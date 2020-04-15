import datetime
import os
import json
import enum
'''
DAQ Controller Brain Class
D. Coderre, 12. Mar. 2019
D. Masson, 06 Apr 2020

Brief: This code handles the logic of what the dispatcher does when. It takes in 
aggregated status updates and commands from the mongo connector and decides if
any action needs to be taken to get the DAQ into the target state. It also handles the
resetting of runs (the ~hourly stop/start) during normal operations.
'''

class STATUS(enum.Enum):
    IDLE = 0
    ARMING = 1
    ARMED = 2
    RUNNING = 3
    ERROR = 4
    TIMEOUT = 5
    UNKNOWN = 6


class DAQController():

    def __init__(self, config, mongo_connector, log):

        self.mongo = mongo_connector
        self.goal_state = {}
        self.latest_status = {}

        # Timeouts. There are a few things that we want to wait for that might take time.
        # The keys for these dicts will be detector identifiers.
        detectors = list(json.loads(config['DEFAULT']['MasterDAQConfig']).keys())
        self.last_command = {k:{d : datetime.datetime.utcnow() for d in detectors}
                                for k in ['arm','start','stop']}
        self.error_stop_count = {d : 0 for d in detectors}

        # Timeout properties come from config
        self.timeouts = {
                k.lower() : int(config['DEFAULT']['%sCommandTimeout' % k])
                for k in ['Arm','Start','Stop']}
        self.stop_retries = int(config['DEFAULT']['RetryReset'])

        self.log = log

        # to give detectors a few more seconds to breathe between runs
        self.skip_one_cycle = {d : False for d in detectors}

    def SolveProblem(self, latest_status, goal_state):
        '''
        This is sort of the whole thing that all the other code is supporting
        We get the status from the DAQ and the command from the user
        Then one of three things can happen:
             1) The status agrees with the command. We're in the goal state and happy.
             2) The status differs from the command. We issue the necessary commands
                to put the system into the goal state
             3) The status and goal are irreconcilable. We complain with an error
                because we can't find any way to put the system into the goal state.
                This could be because a key component is either in error or not present
                at all. Or it could be because we are trying to, for example, start a calibration
                run in the neutron veto but it is already running a combined run and
                therefore unavailable. The frontend should prevent many of these cases though.

        The way that works is this. We do everything iteratively. Like if we see that
        some detector needs to be stopped in order to proceed we issue the stop command
        then move on. Everything is then re-evaluated once that command runs through.

        I wrote this very verbosely since it's got quite a few different possibilities and
        after rewriting once I am convinced longer, clearer code is better than terse, efficient
        code for this particular function. Also I'm hardcoding the detector names. 
        '''

        # cache these so other functions can see them
        self.goal_state = goal_state
        self.latest_status = latest_status

        for det in latest_status.keys():
            if latest_status[det]['status'] == STATUS.IDLE:
                self.error_stop_count[det] = 0

        '''
        CASE 1: DETECTORS ARE INACTIVE

        In our case 'inactive' means 'stopped'. An inactive detector is in its goal state as 
        long as it isn't doing anything, i.e. not ARMING, ARMED, or RUNNING. We don't care if 
        it's idle, or in error, or if there's no status at all. We will care about that later
        if we try to activate it.
        '''
        # 1a - deal with TPC and also with MV and NV, but only if they're linked
        active_states = [STATUS.ARMING, STATUS.ARMED, STATUS.RUNNING, STATUS.UNKNOWN,
                         STATUS.ERROR, STATUS.TIMEOUT]
        if goal_state['tpc']['active'] == 'false':

            # Send stop command if we have to
            if (
                    # TPC not in Idle, error, timeout
                    (latest_status['tpc']['status'] in active_states) or
                    # MV linked and not in Idle, error, timeout
                    (latest_status['muon_veto']['status']  in active_states and
                     goal_state['tpc']['link_mv'] == 'true') or
                    # NV linked and not in Idle, error, timeout
                    (latest_status['neutron_veto']['status']  in active_states and
                     goal_state['tpc']['link_nv'] == 'true')
            ):
                self.ControlDetector(detector='tpc', command='stop')
                #self.CheckTimeouts('tpc')

        # 1b - deal with MV but only if MV not linked to TPC
        if goal_state['tpc']['link_mv'] == 'false' and goal_state['muon_veto']['active'] == 'false':
            if latest_status['muon_veto']['status']  in active_states:
                self.ControlDetector(detector='muon_veto', command='stop')
                #self.CheckTimeouts('muon_veto')
        # 1c - deal with NV but only if NV not linked to TPC
        if goal_state['tpc']['link_nv'] == 'false' and goal_state['neutron_veto']['active'] == 'false':
            if latest_status['neutron_veto']['status']  in active_states:
                self.ControlDetector(detector='neutron_veto', command='stop')
                #self.CheckTimeouts('neutron_veto')

        '''
        CASE 2: DETECTORS ARE ACTIVE

        This will be more complicated.
        There are now 4 possibilities (each with sub-possibilities and each for different
        combinations of linked or unlinked detectors):
         1. The detectors were already running. Here we have to check if the run needs to
            be reset but otherwise maybe we can just do nothing.
         2. The detectors were not already running. We have to start them.
         3. The detectors are in some failed state. We should periodically complain
         4. The detectors are in some in-between state (i.e. ARMING) and we just need to
            wait for some seconds to allow time for the thing to sort itself out.
        '''
        # 2a - again we consider the TPC first, as well as the cases where the NV/MV are linked
        if goal_state['tpc']['active'] == 'true':

            # Maybe we have nothing to do except check the run turnover
            if (
                    # TPC running!
                    (latest_status['tpc']['status'] == STATUS.RUNNING) and
                    # MV either unlinked or running
                    (latest_status['muon_veto']['status'] == STATUS.RUNNING or
                     goal_state['tpc']['link_mv'] == 'false') and
                    # NV either unlinked or running
                    (latest_status['neutron_veto']['status'] == STATUS.RUNNING or
                     goal_state['tpc']['link_nv'] == 'false')
            ):
                self.log.debug("Checking run turnover TPC")
                self.CheckRunTurnover('tpc')

            # Maybe we're already ARMED and should start a run
            elif (
                    # TPC ARMED
                    (latest_status['tpc']['status'] == STATUS.ARMED) and
                    # MV ARMED or UNLINKED
                    (latest_status['muon_veto']['status'] == STATUS.ARMED or
                     goal_state['tpc']['link_mv'] == 'false') and
                    # NV ARMED or UNLINKED
                    (latest_status['neutron_veto']['status'] == STATUS.ARMED or
                     goal_state['tpc']['link_nv'] == 'false')):
                self.log.info("Starting TPC")
                self.ControlDetector(command='start', detector='tpc')

            # Maybe we're IDLE and should arm a run
            elif (
                    # TPC IDLE
                    (latest_status['tpc']['status'] == STATUS.IDLE) and
                    # MV IDLE or UNLINKED
                    (latest_status['muon_veto']['status'] == STATUS.IDLE or
                     goal_state['tpc']['link_mv'] == 'false') and
                    # NV IDLE or UNLINKED
                    (latest_status['neutron_veto']['status'] == STATUS.IDLE or
                     goal_state['tpc']['link_nv'] == 'false')):
                self.log.info("Arming TPC")
                self.ControlDetector(command='arm', detector='tpc')

            elif (
                    # TPC ARMING
                    (latest_status['tpc']['status'] == STATUS.ARMING) and
                    # MV ARMING or UNLINKED
                    (latest_status['muon_veto']['status'] == STATUS.ARMING or
                     goal_state['tpc']['link_mv'] == 'false') and
                    # NV ARMING or UNLINKED
                    (latest_status['neutron_veto']['status'] == STATUS.ARMING or
                     goal_state['tpc']['link_nv'] == 'false')):
                self.CheckTimeouts(detector='tpc', command='arm')

            elif (
                    # TPC ERROR
                    (latest_status['tpc']['status'] == STATUS.ERROR) and
                    # MV ERROR or UNLINKED
                    (latest_status['muon_veto']['status'] == STATUS.ERROR or
                     goal_state['tpc']['link_mv'] == 'false') and
                    # NV ERROR or UNLINKED
                    (latest_status['neutron_veto']['status'] == STATUS.ERROR or
                     goal_state['tpc']['link_nv'] == 'false')):
                self.log.info("TPC has error!")
                self.ControlDetector(command='stop', detector='tpc', force=True)


            # Maybe someone is timing out or we're in some weird mixed state
            # I think this can just be an 'else' because if we're not in some state we're happy
            # with we should probably check if a reset is in order.
            # Note that this will be triggered nearly every run during ARMING so it's not a
            # big deal
            else:
                self.log.debug("Checking TPC timeouts")
                self.CheckTimeouts('tpc')

        # 2b, 2c. In case the MV and/or NV are UNLINKED and ACTIVE we can treat them
        # in basically the same way.
        for detector in ['muon_veto', 'neutron_veto']:
            linked = goal_state['tpc']['link_mv']
            if detector == 'neutron_veto':
                linked = goal_state['tpc']['link_nv']

            # Active/unlinked. You your own detector now.
            if (goal_state[detector]['active'] == 'true' and linked == 'false'):

                # Same logic as before but simpler cause we don't have to check for links
                if latest_status[detector]['status'] == STATUS.RUNNING:
                    self.CheckRunTurnover(detector)
                elif latest_status[detector]['status'] == STATUS.ARMED:
                    self.ControlDetector(command='start', detector=detector)
                elif latest_status[detector]['status'] == STATUS.IDLE:
                    self.ControlDetector(command='arm', detector=detector)
                elif latest_status[detector]['status'] == STATUS.ERROR:
                    self.ControlDetector(command='stop', detector=detector, force=True)
                else:
                    self.CheckTimeouts(detector)

        return


    def ControlDetector(self, command, detector, force=False):
        '''
        Issues the command to the detector if allowed by the timeout
        '''
        now = datetime.datetime.utcnow()
        try:
            dt = (now - self.last_command[command][detector]).total_seconds()
        except (KeyError, TypeError):
            dt = 2*self.timeouts[command]

        if dt > self.timeouts[command] or force:
            run_mode = self.goal_state[detector]['mode']
            if command in ['start','arm']:
                if command == 'arm' and self.skip_one_cycle[detector] == True:
                    self.skip_one_cycle[detector] = False
                    return
                readers, cc = self.mongo.GetHostsForMode(run_mode)
                delay = 0
                if command == 'start':
                    run = self.mongo.InsertRunDoc(detector, self.goal_state)
            else: # stop
                self.skip_one_cycle[detector] = True
                readers, cc = self.mongo.GetConfiguredNodes(detector,
                    self.goal_state['tpc']['link_mv'], self.goal_state['tpc']['link_nv'])
                delay = 5
                # TODO smart delay?
                try:
                    self.mongo.SetStopTime(self.latest_status[detector]['number'])
                except Exception as E:
                    self.log.warning("Wanted to stop run but no associated number, got %s exception: %s" % (type(E), E))
            self.log.debug('Sending %s to %s' % (command.upper(), detector))
            self.mongo.SendCommand(command, cc, self.goal_state[detector]['user'],
                    detector, self.goal_state[detector]['mode'])
            self.mongo.SendCommand(command, readers, self.goal_state[detector]['user'],
                    detector, self.goal_state[detector]['mode'], delay)
            self.last_command[command][detector] = now

        else:
            self.CheckTimeouts(detector=detector, command=command)


    def CheckTimeouts(self, detector, command = None):
        ''' 
        This one is invoked if we think we need to change states. Either a stop command needs
        to be sent, or we've detected an anomaly and want to decide what to do. 
        Basically this function decides:
          - We are not in any timeouts: send the normal stop command
          - We are waiting for something: do nothing
          - We were waiting for something but it took too long: attempt reset
        '''

        sendstop = False
        nowtime = datetime.datetime.utcnow()

        if command is None: # not specified, we figure out it here
            command_times = [(cmd,doc[detector]) for cmd,doc in self.last_command.items()]
            command = sorted(command_times, key=lambda x : x[1])[-1][0]
            self.log.debug('Most recent command for %s is %s' % 
                    (detector, command))
        else:
            self.log.debug('Checking %s timeout for %s' % (command, detector))

        dt = (nowtime - self.last_command[command][detector]).total_seconds()

        local_timeouts = self.timeouts
        local_timeouts['stop'] = self.timeouts['stop']*(self.error_stop_count[detector]+1)

        if dt < local_timeouts[command]:
            self.log.debug('%i is within the %i second timeout for a %s command' %
                    (dt, local_timeouts[command], command))
        else:
            # timing out, maybe send stop?
            if command == 'stop':
                if self.error_stop_count[detector] >= self.stop_retries:
                    # failed too many times, issue error
                    self.mongo.LogError("dispatcher",
                                        ("Dispatcher control loop detects a timeout that is not solved " +
                                         "with a STOP command"),
                                        'ERROR',
                                        "STOP_TIMEOUT")
                    self.error_stop_count[detector] = 0
                else:
                    self.ControlDetector(detector=detector, command='stop')
                    self.log.debug('Working on a stop counter for %s' % detector)
                    self.error_stop_count[detector] += 1
            else:
                self.mongo.LogError("dispatcher",
                        ('%s took more than %i seconds to %s, indicating a possible timeout or error' %
                            (detector, self.timeouts[command], command)),
                        'ERROR',
                        '%s_TIMEOUT' % command.upper())
                self.ControlDetector(detector=detector, command='stop')

        return


    def ThrowError(self):
        '''
        Throw a general error that the DAQ is stuck
        '''
        self.mongo.LogError("dispatcher",
                            "Dispatcher control loop can't get DAQ out of stuck state",
                            'ERROR',
                            "GENERAL_ERROR")

    def CheckRunTurnover(self, detector):
        '''
        During normal operation we want to run for a certain number of minutes, then
        automatically stop and restart the run. No biggie. We check the time here
        to see if it's something we have to do.
        '''
        # If no stop after configured, return
        try:
            _ = int(self.goal_state[detector]['stop_after'])
        except:
            return

        try:
            number = self.latest_status[detector]['number']
        except:
            # dirty workaround just in case there was a dispatcher crash
            self.latest_status[detector]['number'] = self.mongo.GetNextRunNumber() - 1
            number = self.latest_status[detector]['number']
        start_time = self.mongo.GetRunStart(number)
        nowtime = datetime.datetime.utcnow()
        run_length = int(self.goal_state[detector]['stop_after'])*60
        if ((nowtime-start_time).total_seconds() > run_length and
            (nowtime - self.last_command['stop'][detector]).total_seconds() > self.timeouts['stop']):
            self.log.info('Stopping run for %s' % detector)
            self.last_command['stop'][detector] = nowtime
            self.ControlDetector(detector=detector, command='stop')
