import configparser
import argparse
from pymongo import MongoClient
import MongoHelpers
from Detector import Detector
import os
import json
import datetime
import time

HOSTNAME = "fdaq00_dispatcher"
STATUS=["Idle", "Arming", "Armed", "Running", "Error", "Timeout", "Undecided"]


# Parse command line
parser = argparse.ArgumentParser(description='Manage the DAQ')
parser.add_argument('--config', type=str,
                   help='Path to your configuration file')
args = parser.parse_args()
config = configparser.ConfigParser()
try:
    config.read(args.config)
except Exception as e:
    print(e)
    print("Invalid configuration file: %s"%args.config)
    raise

    
# Initialize database connection objects
try:
    logger = MongoHelpers.MongoLog(
        config['DEFAULT']['ControlDatabaseURI']%os.environ["MONGO_PASSWORD"],
        'dax', HOSTNAME)
    control_db = MongoHelpers.ControlDB(
        config['DEFAULT']['ControlDatabaseURI']%os.environ["MONGO_PASSWORD"],
        'dax', logger)    
except Exception as E:
    print("Failed to initialize database objects. Did you set your mongo "
          "password in the MONGO_PASSWORD environment variable? The "
          "exception follows:")
    print(E)
    raise


# Read in all the config
detectors = {}
try:
    detector_config = json.loads(config['DETECTORS']['MasterDAQConfig'])
except Exception as E:
    print("Your detector config is likely invalid JSON. Try running it "
          "through JSONLint (or similar). Exception: ")
    print(E)
    raise
for detector in detector_config.keys():
    detectors[detector] = Detector(detector_config[detector])

node_timeout = config.getint('DEFAULT', 'ClientTimeout')
poll_frequency = config.getint('DEFAULT', 'PollFrequency')



# Main program loop
while(1):
    
    for detector_name, detector in detectors.items():
        detector.status=control_db.GetStatus(detector.readers(), node_timeout)

        print("Detector %s has status %s"%(detector_name, STATUS[detector.status]))

    
    # Check command DB for commands addressed to the dispatcher and
    # process in the order they are received
    command_cursor = control_db.GetDispatcherCommands()
    for doc in command_cursor:
        control_db.ProcessCommand(doc, detectors)

    # Special check to see if any detector is finished arming and should
    # be started
    for detector_name, detector in detectors.items():
        if detector.arming:

            # Arm completed successfully
            if detector.status == 2:
                detector.pending_command['command'] = 'send_start_signal'
                control_db.ProcessCommand(detector.pending_command, detectors)
                detector.clear_arm()
            # Arm timed out
            elif detector.check_arm_fail(datetime.datetime.now().timestamp(), node_timeout):
                logger.entry("Failed to arm detector %s. Arm command timed out."%detector_name,
                             logger.error)
                detector.clear_arm()
            
                
    
    time.sleep(poll_frequency)