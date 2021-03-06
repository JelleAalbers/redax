#include "StraxInserter.hh"
#include <lz4frame.h>
#include "DAQController.hh"
#include "MongoLog.hh"
#include "Options.hh"
#include <blosc.h>
#include <thread>
#include <cstring>
#include <cstdarg>
#include <numeric>
#include <sstream>
#include <list>

namespace fs=std::experimental::filesystem;

StraxInserter::StraxInserter(){
  fOptions = NULL;
  fDataSource = NULL;
  fActive = true;
  fChunkLength=0x7fffffff; // DAQ magic number
  fChunkNameLength=6;
  fChunkOverlap = 0x2FAF080;
  fStraxHeaderSize=24;
  fFragmentBytes=110*2;
  fLog = NULL;
  fErrorBit = false;
  fMissingVerified = 0;
  fOutputPath = "";
  fThreadId = std::this_thread::get_id();
  fBytesProcessed = 0;
  fFragmentSize = 0;
  fForceQuit = false;
  fFullChunkLength = fChunkLength+fChunkOverlap;
  fFragmentsProcessed = 0;
  fEventsProcessed = 0;
}

StraxInserter::~StraxInserter(){
  fActive = false;
  int counter_short = 0, counter_long = 0;
  fLog->Entry(MongoLog::Local, "Thread %lx waiting to stop, has %i events left",
      fThreadId, fBufferLength.load());
  int events_start = fBufferLength.load();
  do{
    events_start = fBufferLength.load();
    while (fRunning && counter_short++ < 500)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (counter_short >= 500)
      fLog->Entry(MongoLog::Message, "Thread %lx taking a while to stop, still has %i evts",
          fThreadId, fBufferLength.load());
    counter_short = 0;
  } while (fRunning && fBufferLength.load() > 0 && events_start > fBufferLength.load() && counter_long++ < 10);
  if (fRunning) {
    fLog->Entry(MongoLog::Warning, "Force-quitting thread %lx: %i events lost",
        fThreadId, fBufferLength.load());
    fForceQuit = true;
    std::this_thread::sleep_for(std::chrono::seconds(2));
  }
  while (fRunning) {
    fLog->Entry(MongoLog::Message, "Still waiting for thread %lx to stop", fThreadId);
    std::this_thread::sleep_for(std::chrono::seconds(2));
  }
  long total_dps = std::accumulate(fBufferCounter.begin(), fBufferCounter.end(), 0,
      [&](long tot, auto& p){return tot + p.second;});
  std::map<std::string, long> counters {
    {"bytes", fBytesProcessed},
    {"fragments", fFragmentsProcessed},
    {"events", fEventsProcessed},
    {"data_packets", total_dps}};
  fOptions->SaveBenchmarks(counters, fBufferCounter,
      fProcTime.count(), fCompTime.count());
}

int StraxInserter::Initialize(Options *options, MongoLog *log, DAQController *dataSource,
			      std::string hostname){
  fOptions = options;
  fChunkLength = long(fOptions->GetDouble("strax_chunk_length", 5)*1e9); // default 5s
  fChunkOverlap = long(fOptions->GetDouble("strax_chunk_overlap", 0.5)*1e9); // default 0.5s
  fFragmentBytes = fOptions->GetInt("strax_fragment_payload_bytes", 110*2);
  fCompressor = fOptions->GetString("compressor", "lz4");
  fFullChunkLength = fChunkLength+fChunkOverlap;
  fHostname = hostname;
  std::string run_name = fOptions->GetString("run_identifier", "run");

  fMissingVerified = 0;
  fDataSource = dataSource;
  dataSource->GetDataFormat(fFmt);
  fLog = log;
  fErrorBit = false;

  fProcTime = std::chrono::microseconds(0);
  fCompTime = std::chrono::microseconds(0);

  std::string output_path = fOptions->GetString("strax_output_path", "./");
  try{    
    fs::path op(output_path);
    op /= run_name;
    fOutputPath = op;
    fs::create_directory(op);
  }
  catch(...){
    fLog->Entry(MongoLog::Error, "StraxInserter::Initialize tried to create output directory but failed. Check that you have permission to write here.");
    return -1;
  }
  //fLog->Entry(MongoLog::Local, "Strax output initialized with %li ns chunks and %li ns overlap time",
  //  fChunkLength, fChunkOverlap);

  return 0;
}

void StraxInserter::Close(std::map<int,int>& ret){
  fActive = false;
  const std::lock_guard<std::mutex> lg(fFC_mutex);
  for (auto& iter : fFailCounter) ret[iter.first] += iter.second;
}

void StraxInserter::GetDataPerChan(std::map<int, int>& ret) {
  if (!fActive) return;
  fDPC_mutex.lock();
  for (auto& pair : fDataPerChan) {
    ret[pair.first] += pair.second;
    pair.second = 0;
  }
  fDPC_mutex.unlock();
  return;
}

void StraxInserter::GenerateArtificialDeadtime(int64_t timestamp, int16_t bid) {
  std::string fragment;
  fragment.append((char*)&timestamp, sizeof(timestamp));
  int32_t length = fFragmentBytes>>1;
  fragment.append((char*)&length, sizeof(length));
  int16_t sw = 10;
  fragment.append((char*)&sw, sizeof(sw));
  int16_t channel = 790; // TODO add MV and NV support
  fragment.append((char*)&channel, sizeof(channel));
  fragment.append((char*)&length, sizeof(length));
  int16_t fragment_i = 0;
  fragment.append((char*)&fragment_i, sizeof(fragment_i));
  int16_t baseline = 0;
  fragment.append((char*)&baseline, sizeof(baseline));
  fragment.append((char*)&bid, sizeof(bid));
  int8_t zero = 0;
  while ((int)fragment.size() < fFragmentBytes+fStraxHeaderSize)
    fragment.append((char*)&zero, sizeof(zero));
  AddFragmentToBuffer(fragment, timestamp);
}

void StraxInserter::ParseDocuments(data_packet* dp){

  using namespace std::chrono;
  system_clock::time_point proc_start, proc_end;

  // Take a buffer and break it up into one document per channel
  unsigned int max_channels = 16; // hardcoded to accomodate V1730

  // Unpack the things from the data packet
  std::vector<u_int32_t> clock_counters(max_channels, dp->clock_counter);
  std::vector<u_int32_t> last_times_seen(max_channels, 0xFFFFFFFF);

  u_int32_t size = dp->size;
  u_int32_t *buff = dp->buff;
  int smallest_latest_index_seen = -1;
  const int event_header_words = 4;

  u_int32_t idx = 0;
  std::map<std::string, int> fmt = fFmt[dp->bid];
  std::map<int, int> data_per_chan;
  unsigned total_words = size/sizeof(u_int32_t);
  proc_start = system_clock::now();
  while(idx < total_words && buff[idx] != 0xFFFFFFFF){
    
    if(buff[idx]>>28 == 0xA){ // 0xA indicates header at those bits

      // Get data from main header
      u_int32_t words_in_event = std::min(buff[idx]&0xFFFFFFF, total_words-idx);
      u_int32_t channel_mask = (buff[idx+1]&0xFF);

      if (words_in_event < (buff[idx]&0xFFFFFFF)) {
        fLog->Entry(MongoLog::Local, "Board %i garbled event header at idx %i: %u/%u (%i)",
            dp->bid, idx, buff[idx]&0xFFFFFFF, total_words-idx, dp->vBLT.size());
      }

      if (fmt["channel_mask_msb_idx"] != -1) {
	channel_mask = ( ((buff[idx+2]>>24)&0xFF)<<8 ) | (buff[idx+1]&0xFF);
      }
      
      // Exercise for the reader: if you're modifying for V1730 add in the rest of the bits here!
      u_int32_t channels_in_event = __builtin_popcount(channel_mask);
      bool board_fail = buff[idx+1]&0x4000000; // & (buff[idx+1]>>27)
      u_int32_t event_time = buff[idx+3]&0xFFFFFFFF;
      fEventsProcessed++;

      if(board_fail){
        const std::lock_guard<std::mutex> lg(fFC_mutex);
        // would generate deadtime here but no timestamp
        fDataSource->CheckError(dp->bid);
	fFailCounter[dp->bid]++;
        idx += event_header_words;
        continue;
      }
      unsigned event_start_idx = idx;
      idx += event_header_words; // skip header

      for(unsigned int channel=0; channel<max_channels; channel++){
	if(!((channel_mask>>channel)&1))
	  continue;

	// These defaults are valid for 'default' firmware where all channels same size
	u_int32_t channel_words = (words_in_event-event_header_words) / channels_in_event;
	u_int32_t channel_time = event_time;
	u_int32_t channel_timeMSB = 0;
	u_int16_t baseline_ch = 0;
        bool whoops = false;

	// Presence of a channel header indicates non-default firmware (DPP-DAW) so override
	if(fmt["channel_header_words"] > 0){
	  channel_words = std::min(buff[idx]&0x7FFFFF, words_in_event - (idx - event_start_idx));
          if (channel_words < (buff[idx]&0x7FFFFF)) {
            fLog->Entry(MongoLog::Local, "Board %i ch %i garbled header at idx %i: %x/%x",
                  dp->bid, channel, idx, buff[idx]&0x7FFFFF, words_in_event);
            idx += fmt["channel_header_words"];
            break;
          }
          if (channel_words <= fmt["channel_header_words"]) {
            fLog->Entry(MongoLog::Local, "Board %i ch %i empty (%i/%i)",
                dp->bid, channel, channel_words, fmt["channel_header_words"]);
            idx += (fmt["channel_header_words"]-channel_words);
            continue;
          }
          channel_words -= fmt["channel_header_words"];
	  channel_time = buff[idx+1]&0xFFFFFFFF;

	  if (fmt["channel_time_msb_idx"] == 2) {
	    channel_timeMSB = buff[idx+2]&0xFFFF;
	    baseline_ch = (buff[idx+2]>>16)&0x3FFF;
	  }
	  
	  idx += fmt["channel_header_words"];

	  // V1724 only. 1730 has a **26-day** clock counter. 
	  if(fmt["channel_header_words"] <= 2){    
	    // OK. Here's the logic for the clock reset, and I realize this is the
	    // second place in the code where such weird logic is needed but that's it
	    // First, on the first instance of a channel we gotta check if
	    // the channel clock rolled over BEFORE this clock and adjust the counter
	    
	    if(channel_time > 15e8 && dp->header_time<5e8 &&
	       last_times_seen[channel] == 0xFFFFFFFF && clock_counters[channel]!=0){
	      clock_counters[channel]--;
	    }
	    // Now check the opposite
	    else if(channel_time <5e8 && dp->header_time > 15e8 &&
		    last_times_seen[channel] == 0xFFFFFFFF){
	      clock_counters[channel]++;
	    }
	    
	    // Now check if this time < last time (indicates rollover)
	    if(channel_time < last_times_seen[channel] &&
	       last_times_seen[channel]!=0xFFFFFFFF)
	      clock_counters[channel]++;

	    last_times_seen[channel] = channel_time;
	    
	  }
	} // channel_header_words > 0

        // let's sanity-check the data first to make sure we didn't get CAENed
        for (unsigned w = 0; w < channel_words; w++) {
          if ((idx+w >= total_words) || (buff[idx+w]>>28) == 0xA) {
            fLog->Entry(MongoLog::Local, "Board %i has CAEN'd itself at idx %x",
                dp->bid, idx+w);
            whoops = true;
            break;
          }
        }
        if (idx - event_start_idx >= words_in_event) {
          fLog->Entry(MongoLog::Local, "Board %i CAEN'd itself at idx %x",
              dp->bid, idx);
          whoops = true;
        }

	// Exercise for reader. This is for our 30-bit trigger clock. If yours was, say,
	// 48 bits this line would be different
	int iBitShift = 31;
	int64_t Time64;

	 if (fmt["channel_time_msb_idx"] == 2) { 
	   Time64 = fmt["ns_per_clk"]*( ( (unsigned long)channel_timeMSB<<(int)32) + channel_time); 
	 } else {
	   Time64 = fmt["ns_per_clk"]*(((unsigned long)clock_counters[channel] <<
					      iBitShift) + channel_time); // in ns
	}

        if (whoops) { // some data got lost somewhere
          GenerateArtificialDeadtime(Time64, dp->bid);
          break; // loop over channels
        }

	// We're now at the first sample of the channel's waveform. This
	// will be beautiful. First we reinterpret the channel as 16
	// bit because we want to allow also odd numbers of samples
	// as FragmentLength
	u_int16_t *payload = reinterpret_cast<u_int16_t*>(buff);
	u_int32_t samples_in_pulse = channel_words<<1;
	u_int32_t index_in_pulse = 0;
	u_int32_t offset = idx<<1;
	u_int16_t fragment_index = 0;
	u_int16_t sw = fmt["ns_per_sample"];
        int fragment_samples = fFragmentBytes>>1;
	int16_t cl = fOptions->GetChannel(dp->bid, channel);
        data_per_chan[cl] += samples_in_pulse<<1;
	// Failing to discern which channel we're getting data from seems serious enough to throw
	if(cl==-1)
	  throw std::runtime_error("Failed to parse channel map. I'm gonna just kms now.");
          
	
	while(index_in_pulse < samples_in_pulse){
	  std::string fragment;
	  
	  // How long is this fragment?
	  u_int32_t max_sample = index_in_pulse + fragment_samples;
	  u_int32_t samples_this_fragment = fragment_samples;
	  if((unsigned int)(fragment_samples + (fragment_index*fragment_samples)) >
	     samples_in_pulse){
	    max_sample = index_in_pulse + (samples_in_pulse -
					    (fragment_index*fragment_samples));
	    samples_this_fragment = max_sample-index_in_pulse;
	  }
          fFragmentsProcessed++;

	  u_int64_t time_this_fragment = Time64 + fragment_samples*sw*fragment_index;
	  char *pulseTime = reinterpret_cast<char*> (&time_this_fragment);
	  fragment.append(pulseTime, 8);

	  char *fragmentlength = reinterpret_cast<char*> (&samples_this_fragment);
	  fragment.append(fragmentlength, 4);

	  char *sampleWidth = reinterpret_cast<char*> (&sw);
	  fragment.append(sampleWidth, 2);

	  char *channelLoc = reinterpret_cast<char*> (&cl);
	  fragment.append(channelLoc, 2);

	  char *samplesinpulse = reinterpret_cast<char*> (&samples_in_pulse);
	  fragment.append(samplesinpulse, 4);

	  char *fragmentindex = reinterpret_cast<char*> (&fragment_index);
	  fragment.append(fragmentindex, 2);

          char* bl = reinterpret_cast<char*>(&baseline_ch);
          fragment.append(bl, 2);

	  // Copy the raw buffer
	  const char *data_loc = reinterpret_cast<const char*>(&(payload[offset+index_in_pulse]));
	  fragment.append(data_loc, samples_this_fragment*2);
          uint8_t zero_filler = 0;
          char *zero = reinterpret_cast<char*> (&zero_filler);
	  while((int)fragment.size()<fFragmentBytes+fStraxHeaderSize)
	    fragment.append(zero, 1); // int(0) != int("0")

          int chunk_id = AddFragmentToBuffer(fragment, time_this_fragment);

	  // Check if this is the smallest_latest_index_seen
	  if(smallest_latest_index_seen == -1 || chunk_id < smallest_latest_index_seen)
	    smallest_latest_index_seen = chunk_id;
	
	  fragment_index++;
	  index_in_pulse = max_sample;
          if (fForceQuit == true) break;
	} // while in pulse
	idx+=channel_words;
        if (fForceQuit == true) break;
      } // channel loop
      if (fForceQuit == true) break;
    } // if header
    else
      idx++;
  }
  fDPC_mutex.lock();
  for (auto& p : data_per_chan) fDataPerChan[p.first] += p.second;
  fDPC_mutex.unlock();
  proc_end = system_clock::now();
  if(smallest_latest_index_seen != -1)
    WriteOutFiles(smallest_latest_index_seen);

  fBytesProcessed += dp->size;
  fProcTime += duration_cast<microseconds>(proc_end - proc_start);
  delete dp;
}

int StraxInserter::AddFragmentToBuffer(std::string& fragment, int64_t timestamp) {
  // Get the CHUNK and decide if this event also goes into a PRE/POST file
  int chunk_id = timestamp/fFullChunkLength;
  bool nextpre = (chunk_id+1)* fFullChunkLength - timestamp < fChunkOverlap;
  // Minor mess to maintain the same width of file names and do the pre/post stuff
  // If not in pre/post
  std::string chunk_index = std::to_string(chunk_id);
  while(chunk_index.size() < fChunkNameLength)
    chunk_index.insert(0, "0");

  fFragmentSize += fragment.size();

  if(!nextpre){
    if(fFragments.count(chunk_index) == 0){
      fFragments[chunk_index] = new std::string();
    }
    fFragments[chunk_index]->append(fragment);
  } else {
    std::string nextchunk_index = std::to_string(chunk_id+1);
    while(nextchunk_index.size() < fChunkNameLength)
      nextchunk_index.insert(0, "0");

    if(fFragments.count(nextchunk_index+"_pre") == 0){
      fFragments[nextchunk_index+"_pre"] = new std::string();
    }
    fFragments[nextchunk_index+"_pre"]->append(fragment);

    if(fFragments.count(chunk_index+"_post") == 0){
      fFragments[chunk_index+"_post"] = new std::string();
    }
    fFragments[chunk_index+"_post"]->append(fragment);
  }
  return chunk_id;
}


int StraxInserter::ReadAndInsertData(){
  fThreadId = std::this_thread::get_id();
  fActive = fRunning = true;
  fBufferLength = 0;
  std::chrono::microseconds sleep_time(10);
  if (fOptions->GetString("buffer_type", "dual") == "dual") {
    while(fActive == true){
      std::list<data_packet*> b;
      if (fDataSource->GetData(&b)) {
        fBufferLength = b.size();
        fBufferCounter[int(b.size())]++;
        for (auto& dp_ : b) {
          ParseDocuments(dp_);
          fBufferLength--;
          dp_ = nullptr;
          if (fForceQuit) break;
        }
        if (fForceQuit) for (auto& dp_ : b) if (dp_ != nullptr) delete dp_;
        b.clear();
      } else {
        std::this_thread::sleep_for(sleep_time);
      }
    }
  } else {
    data_packet* dp;
    while (fActive == true) {
      if (fDataSource->GetData(dp)) {
        fBufferLength = 1;
        fBufferCounter[1]++;
        ParseDocuments(dp);
        fBufferLength = 0;
      } else {
        std::this_thread::sleep_for(sleep_time);
      }
    }
  }
  if (fBytesProcessed > 0)
    WriteOutFiles(1000000, true);
  fRunning = false;
  return 0;
}

// Can tune here as needed, these are defaults from the LZ4 examples
static const LZ4F_preferences_t kPrefs = {
  { LZ4F_max256KB, LZ4F_blockLinked, LZ4F_noContentChecksum, LZ4F_frame, 0, { 0, 0 } },
    0,   /* compression level; 0 == default */
    0,   /* autoflush */
    { 0, 0, 0 },  /* reserved, must be set to 0 */
};

void StraxInserter::WriteOutFiles(int smallest_index_seen, bool end){
  // Write the contents of fFragments to blosc-compressed files
  using namespace std::chrono;
  system_clock::time_point comp_start, comp_end;
  std::vector<std::string> idx_to_clear;
  for (auto& iter : fFragments) {
    if (iter.first == "")
        break; // not sure why, but this sometimes happens during bad shutdowns
    std::string chunk_index = iter.first;
    std::string idnr = chunk_index.substr(0, fChunkNameLength);
    int idnrint = std::stoi(idnr);
    if(!(idnrint < smallest_index_seen-1 || end))
      continue;

    comp_start = system_clock::now();
    if(!fs::exists(GetDirectoryPath(chunk_index, true)))
      fs::create_directory(GetDirectoryPath(chunk_index, true));

    size_t uncompressed_size = iter.second->size();

    // Compress it
    char *out_buffer = NULL;
    int wsize = 0;
    if(fCompressor == "blosc"){
      out_buffer = new char[uncompressed_size+BLOSC_MAX_OVERHEAD];
      wsize = blosc_compress_ctx(5, 1, sizeof(char), uncompressed_size,  iter.second->data(),
				   out_buffer, uncompressed_size+BLOSC_MAX_OVERHEAD, "lz4", 0, 2);
    }
    else{
      // Note: the current package repo version for Ubuntu 18.04 (Oct 2019) is 1.7.1, which is
      // so old it is not tracked on the lz4 github. The API for frame compression has changed
      // just slightly in the meantime. So if you update and it breaks you'll have to tune at least
      // the LZ4F_preferences_t object to the new format.
      size_t max_compressed_size = LZ4F_compressFrameBound(uncompressed_size, &kPrefs);
      out_buffer = new char[max_compressed_size];
      wsize = LZ4F_compressFrame(out_buffer, max_compressed_size,
				 iter.second->data(), uncompressed_size, &kPrefs);
    }
    delete iter.second;
    iter.second = nullptr;
    fFragmentSize -= uncompressed_size;
    idx_to_clear.push_back(chunk_index);

    std::ofstream writefile(GetFilePath(chunk_index, true), std::ios::binary);
    writefile.write(out_buffer, wsize);
    delete[] out_buffer;
    writefile.close();

    // Move this chunk from *_TEMP to the same path without TEMP
    if(!fs::exists(GetDirectoryPath(chunk_index, false)))
      fs::create_directory(GetDirectoryPath(chunk_index, false));
    fs::rename(GetFilePath(chunk_index, true),
	       GetFilePath(chunk_index, false));
    comp_end = system_clock::now();
    fCompTime += duration_cast<microseconds>(comp_end-comp_start);
    
    CreateMissing(idnrint);
  } // End for through fragments
  // clear now because c++ sometimes overruns its buffers
  for (auto s : idx_to_clear) {
    if (fFragments.count(s) != 0) fFragments.erase(s);
  }

  if(end){
    for (auto& p : fFragments)
        if (p.second != nullptr) delete p.second;
    fFragments.clear();
    fFragmentSize = 0;
    fs::path write_path(fOutputPath);
    std::string filename = fHostname;
    write_path /= "THE_END";
    if(!fs::exists(write_path)){
      fLog->Entry(MongoLog::Local,"Creating END directory at %s",write_path.c_str());
      try{
        fs::create_directory(write_path);
      }
      catch(...){};
    }
    std::stringstream ss;
    ss<<std::this_thread::get_id();
    write_path /= fHostname + "_" + ss.str();
    std::ofstream outfile;
    outfile.open(write_path, std::ios::out);
    outfile<<"...my only friend";
    outfile.close();
  }

}

std::string StraxInserter::GetStringFormat(int id){
  std::string chunk_index = std::to_string(id);
  while(chunk_index.size() < fChunkNameLength)
    chunk_index.insert(0, "0");
  return chunk_index;
}

fs::path StraxInserter::GetDirectoryPath(std::string id, bool temp){
  fs::path write_path(fOutputPath);
  write_path /= id;
  if(temp)
    write_path+="_temp";
  return write_path;
}

fs::path StraxInserter::GetFilePath(std::string id, bool temp){
  fs::path write_path = GetDirectoryPath(id, temp);
  std::string filename = fHostname;
  std::stringstream ss;
  ss<<std::this_thread::get_id();
  filename += "_";
  filename += ss.str();
  write_path /= filename;
  return write_path;
}

void StraxInserter::CreateMissing(u_int32_t back_from_id){

  for(unsigned int x=fMissingVerified; x<back_from_id; x++){
    std::string chunk_index = GetStringFormat(x);
    std::string chunk_index_pre = chunk_index+"_pre";
    std::string chunk_index_post = chunk_index+"_post";
    if(!fs::exists(GetFilePath(chunk_index, false))){
      if(!fs::exists(GetDirectoryPath(chunk_index, false)))
	fs::create_directory(GetDirectoryPath(chunk_index, false));
      std::ofstream o;
      o.open(GetFilePath(chunk_index, false));
      o.close();
    }
    if(x!=0 && !fs::exists(GetFilePath(chunk_index_pre, false))){
      if(!fs::exists(GetDirectoryPath(chunk_index_pre, false)))
	fs::create_directory(GetDirectoryPath(chunk_index_pre, false));
      std::ofstream o;
      o.open(GetFilePath(chunk_index_pre, false));
      o.close();
    }
    if(!fs::exists(GetFilePath(chunk_index_post, false))){
      if(!fs::exists(GetDirectoryPath(chunk_index_post, false)))
	fs::create_directory(GetDirectoryPath(chunk_index_post, false));
      std::ofstream o;
      o.open(GetFilePath(chunk_index_post, false));
      o.close();
    }
  }
  fMissingVerified = back_from_id;
}


data_packet::data_packet() {
  buff = nullptr;
  size = 0;
  clock_counter = 0;
  header_time = 0;
  bid = 0;
}

data_packet::~data_packet() {
  if (buff != nullptr) delete[] buff;
  buff = nullptr;
  size = clock_counter = header_time = bid = 0;
  //vBLT.clear();
}
