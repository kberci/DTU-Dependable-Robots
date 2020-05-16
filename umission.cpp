/*************************************************************************** * Copyright (C) 2016-2020 by DTU (Christian Andersen) * * 
 jca@elektro.dtu.dk * * * * This program is free software; you can redistribute it and/or modify * * it under the terms of the GNU 
 Lesser General Public License as * * published by the Free Software Foundation; either version 2 of the * * License, or (at your 
 option) any later version.  * * * * This program is distributed in the hope that it will be useful, * * but WITHOUT ANY WARRANTY; 
 without even the implied warranty of * * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the * * GNU Lesser General Public 
 License for more details.  * * * * You should have received a copy of the GNU Lesser General Public * * License along with this 
 program; if not, write to the * * Free Software Foundation, Inc., * * 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.  * 
 ***************************************************************************/



#include <sys/time.h>
#include <cstdlib>
#include "umission.h"
#include "utime.h"
#include "ulibpose2pose.h"


UMission::UMission(UBridge * regbot, UCamera * camera)
{
  cam = camera;
  bridge = regbot;
  threadActive = 100;
  // initialize line list to empty
  for (int i = 0; i < missionLineMax; i++)
  { // add to line list 
    lines[i] = lineBuffer[i];    
    // terminate c-strings strings - good practice, but not needed
    lines[i][0] = '\0';
  }
  // start mission thread
  th1 = new thread(runObj, this);
}


UMission::~UMission()
{
  printf("Mission class destructor\n");
}


void UMission::run()
{
  while (not active and not th1stop)
    usleep(100000);
//   printf("UMission::run:  active=%d, th1stop=%d\n", active, th1stop);
  if (not th1stop)
    runMission();
  printf("UMission::run: mission thread ended\n");
}
  
void UMission::printStatus()
{
  printf("# ------- Mission ----------\n");
  printf("# active = %d, finished = %d\n", active, finished);
  printf("# mission part=%d, in state=%d\n", mission, missionState);
}
  
/**
 * Initializes the communication with the robobot_bridge and the REGBOT.
 * It further initializes a (maximum) number of mission lines 
 * in the REGBOT microprocessor. */
void UMission::missionInit()
{ // stop any not-finished mission
  bridge->send("robot stop\n");
  // clear old mission
  bridge->send("robot <clear\n");
  //
  // add new mission with 3 threads
  // one (100) starting at event 30 and stopping at event 31
  // one (101) starting at event 31 and stopping at event 30
  // one (  1) used for idle and initialisation of hardware
  // the mission is started, but staying in place (velocity=0, so servo action)
  //
  bridge->send("robot <add thread=1\n");
  // Irsensor should be activated a good time before use 
  // otherwise first samples will produce "false" positive (too short/negative).
  bridge->send("robot <add irsensor=1,vel=0:dist<0.2\n");
  //
  // alternating threads (100 and 101, alternating on event 30 and 31 (last 2 events)
  bridge->send("robot <add thread=100,event=30 : event=31\n");
  for (int i = 0; i < missionLineMax; i++)
    // send placeholder lines, that will never finish
    // are to be replaced with real mission
    // NB - hereafter no lines can be added to these threads, just modified
    bridge->send("robot <add vel=0 : time=0.1\n");
  //
  bridge->send("robot <add thread=101,event=31 : event=30\n");
  for (int i = 0; i < missionLineMax; i++)
    // send placeholder lines, that will never finish
    bridge->send("robot <add vel=0 : time=0.1\n");
  usleep(10000);
  //
  //
  // send subscribe to bridge
  bridge->pose->subscribe();
  bridge->edge->subscribe();
  bridge->motor->subscribe();
  bridge->event->subscribe();
  bridge->joy->subscribe();
  bridge->motor->subscribe();
  bridge->info->subscribe();
  bridge->irdist->subscribe();
  bridge->imu->subscribe();
  usleep(10000);
  // there maybe leftover events from last mission
  bridge->event->clearEvents();
}


void UMission::sendAndActivateSnippet(char ** missionLines, int missionLineCnt)
{
  // Calling sendAndActivateSnippet automatically toggles between thread 100 and 101. 
  // Modifies the currently inactive thread and then makes it active. 
  const int MSL = 100;
  char s[MSL];
  int threadToMod = 101;
  int startEvent = 31;
  // select Regbot thread to modify
  // and event to activate it
  if (threadActive == 101)
  {
    threadToMod = 100;
    startEvent = 30;
  }
  if (missionLineCnt > missionLineMax)
  {
    printf("# ----------- error - too many lines ------------\n");
    printf("# You tried to send %d lines, but there is buffer space for %d only!\n", missionLineCnt, missionLineMax);
    printf("# set 'missionLineMax' to a higher number in 'umission.h' about line 57\n");
    printf("# (not all lines will be send)\n");
    printf("# -----------------------------------------------\n");
    missionLineCnt = missionLineMax;
  }
  // send mission lines using '<mod ...' command
  for (int i = 0; i < missionLineCnt; i++)
  { // send lines one at a time
    if (strlen((char*)missionLines[i]) > 0)
    { // send a modify line command
      snprintf(s, MSL, "<mod %d %d %s\n", threadToMod, i+1, missionLines[i]);
      bridge->send(s); 
    }
    else
      // an empty line will end code snippet too
      break;
  }
  // let it sink in (10ms)
  usleep(10000);
  // Activate new snippet thread and stop the other  
  snprintf(s, MSL, "<event=%d\n", startEvent);
  bridge->send(s);
  // save active thread number
  threadActive = threadToMod;
}


//////////////////////////////////////////////////////////

/**
 * Thread for running the mission(s)
 * All missions segments are called in turn based on mission number
 * Mission number can be set at parameter when starting mission command line.
 * 
 * The loop also handles manual override for the gamepad, and resumes
 * when manual control is released.
 * */
void UMission::runMission()
{ /// current mission number
  mission = fromMission;
  int missionOld = mission;
  bool regbotStarted = false;
  /// end flag for current mission
  bool ended = false;
  /// manuel override - using gamepad
  bool inManual = false;
  /// debug loop counter
  int loop = 0;
  // keeps track of mission state
  missionState = 0;
  int missionStateOld = missionState;
  // fixed string buffer
  const int MSL = 120;
  char s[MSL];
  /// initialize robot mission to do nothing (wait for mission lines)
  missionInit();
  /// start (the empty) mission, ready for mission snippets.
  bridge->send("start\n"); // ask REGBOT to start controlled run (ready to execute)
  bridge->send("oled 3 waiting for REGBOT\n");
  ///
  for (int i = 0; i < 3; i++)
  {
    if (not bridge->info->isHeartbeatOK())
    { // heartbeat should come at least once a second
      sleep(1);
    }
  }
  if (not bridge->info->isHeartbeatOK())
  { // heartbeat should come at least once a second
    system("espeak \"Oops, no usable connection with robot.\" -ven+f4 -s130 -a60 2>/dev/null &"); 
    bridge->send("oled 3 Oops: Lost REGBOT!");
    printf("# ---------- error ------------\n");
    printf("# No heartbeat from robot. Bridge or REGBOT is stuck\n");
    printf("# You could try restart ROBOBOT bridge ('b' from mission console) \n");
    printf("# -----------------------------\n");
    stop();
  }
  /// loop in sequence every mission until they report ended
  while (not finished and not th1stop)
  { // stay in this mission loop until finished
    loop++;
    // test for manuel override (joy is short for joystick or gamepad)
    if (bridge->joy->manual)
    { // just wait, do not continue mission
      usleep(20000);
      if (not inManual)
        system("espeak \"Mission paused.\" -ven+f4 -s130 -a40 2>/dev/null &"); 
      inManual = true;
      bridge->send("oled 3 GAMEPAD control\n");
    }
    else
    { // in auto mode
      if (not regbotStarted)
      { // wait for start event is received from REGBOT
        // - in response to 'bot->send("start\n")' earlier
        if (bridge->event->isEventSet(33))
        { // start mission (button pressed)
//           printf("Mission::runMission: starting mission (part from %d to %d)\n", fromMission, toMission);
          regbotStarted = true;
        }
      }
      else
      { // mission in auto mode
        if (inManual)
        { // just entered auto mode, so tell.
          inManual = false;
          system("espeak \"Mission resuming.\" -ven+f4 -s130 -a40 2>/dev/null &");
          bridge->send("oled 3 running AUTO\n");
        }
        switch(mission)
        {
          case 1: // running auto mission
            ended = mission1(missionState);
            break;
          /*case 2:
            ended = mission2(missionState);
            break;
          case 3:
            ended = mission3(missionState);
            break;*/
          case 2:
            ended = mission4(missionState);
            break;
          default:
            // no more missions - end everything
            finished = true;
            break;
        }
        if (ended)
        { // start next mission part in state 0
          mission++;
          ended = false;
          missionState = 0;
        }
        // show current state on robot display
        if (mission != missionOld or missionState != missionStateOld)
        { // update small O-led display on robot - when there is a change
          UTime t;
          t.now();
          snprintf(s, MSL, "oled 4 mission %d state %d\n", mission, missionState);
          bridge->send(s);
          if (logMission != NULL)
          {
            fprintf(logMission, "%ld.%03ld %d %d\n", 
                    t.getSec(), t.getMilisec(),
                    missionOld, missionStateOld
            );
            fprintf(logMission, "%ld.%03ld %d %d\n", 
                    t.getSec(), t.getMilisec(),
                    mission, missionState
            );
          }
          missionOld = mission;
          missionStateOld = missionState;
        }
      }
    }
    //
    // check for general events in all modes
    // gamepad buttons 0=green, 1=red, 2=blue, 3=yellow, 4=LB, 5=RB, 6=back, 7=start, 8=Logitech, 9=A1, 10 = A2
    // gamepad axes    0=left-LR, 1=left-UD, 2=LT, 3=right-LR, 4=right-UD, 5=RT, 6=+LR, 7=+-UD
    // see also "ujoy.h"
    if (bridge->joy->button[BUTTON_RED])
    { // red button -> save image
      if (not cam->saveImage)
      {
        printf("UMission::runMission:: button 1 (red) pressed -> save image\n");
        cam->saveImage = true;
      }
    }
    if (bridge->joy->button[BUTTON_YELLOW])
    { // yellow button -> make ArUco analysis
      if (not cam->doArUcoAnalysis)
      {
        printf("UMission::runMission:: button 3 (yellow) pressed -> do ArUco\n");
        cam->doArUcoAnalysis = true;
      }
    }
    // are we finished - event 0 disables motors (e.g. green button)
    if (bridge->event->isEventSet(0))
    { // robot say stop
      finished = true;
      printf("Mission:: insist we are finished\n");
    }
    else if (mission > toMission)
    { // stop robot
      // make an event 0
      bridge->send("stop\n");
      // stop mission loop
      finished = true;
    }
    // release CPU a bit (10ms)
    usleep(10000);
  }
  bridge->send("stop\n");
  snprintf(s, MSL, "espeak \"%s finished.\"  -ven+f4 -s130 -a12  2>/dev/null &", bridge->info->robotname);
  system(s); 
  printf("Mission:: all finished\n");
  bridge->send("oled 3 finished\n");
}


////////////////////////////////////////////////////////////

/**
 * Run mission
 * \param state is kept by caller, but is changed here
 *              therefore defined as reference with the '&'.
 *              State will be 0 at first call.
 * \returns true, when finished. */
/*bool UMission::mission1(int & state)
{
  bool finished = false;
  // First commands to send to robobot in given mission
  // (robot sends event 1 after driving 1 meter)):
  switch (state)
  {
    case 0:
      // tell the operatior what to do
      printf("# press green to start.\n");
      system("espeak \"press green to start\" -ven+f4 -s130 -a5 2>/dev/null &"); 
      bridge->send("oled 5 press green to start");
      state++;
      break;
    case 1:
      if (bridge->joy->button[BUTTON_GREEN])
        state = 10;
      break;
    case 10: // first PART - wait for IR2 then go fwd and turn
      snprintf(lines[0], MAX_LEN, "vel=0 : ir2 < 0.3");
      // drive straight 0.6m - keep an acceleration limit of 1m/s2 (until changed)
      snprintf(lines[1], MAX_LEN, "vel=0.2,acc=1:dist=0.6");
      // stop and create an event when arrived at this line
      snprintf(lines[2], MAX_LEN, "event=1, vel=0");
      // add a line, so that the robot is occupied until next snippet has arrived
      snprintf(lines[3], MAX_LEN, ": dist=1");
      // send the 4 lines to the REGBOT
      sendAndActivateSnippet(lines, 4);
      // make sure event 1 is cleared
      bridge->event->isEventSet(1);
      // tell the operator
      printf("# case=%d sent mission snippet 1\n", state);
      system("espeak \"code snippet 1.\" -ven+f4 -s130 -a5 2>/dev/null &"); 
      bridge->send("oled 5 code snippet 1");
      //
      // play as we go
      play.setFile("../The_thing_goes_Bassim.mp3");
      play.setVolume(5); // % (0..100)
      play.start();
      // go to wait for finished
      state = 11;
      // state = 999; // BERCI
      featureCnt = 0;
      break;
    case 11:
      // wait for event 1 (send when finished driving first part)
      if (bridge->event->isEventSet(1))
      { // finished first drive
        state = 999;
        play.stopPlaying();
      }
      break;
    case 999:
    default:
      printf("mission 1 ended \n");
      bridge->send("oled 5 \"mission 1 ended.\"");
      finished = true;
      break;
  }
  return finished;
}*/


/**
 * Run mission
 * \param state is kept by caller, but is changed here
 *              therefore defined as reference with the '&'.
 *              State will be 0 at first call.
 * \returns true, when finished. */
/*bool UMission::mission2(int & state)
{
  bool finished = false;
  // First commands to send to robobot in given mission
  // (robot sends event 1 after driving 1 meter)):
  switch (state)
  {
    case 0:
      // tell the operatior what to do
      printf("# started mission 2.\n");
      system("espeak \"looking for ArUco\" -ven+f4 -s130 -a5 2>/dev/null &"); 
      bridge->send("oled 5 looking 4 ArUco");
      state=11;
      break;
    case 11:
      // wait for finished driving first part)
      if (fabsf(bridge->motor->getVelocity()) < 0.001 and bridge->imu->turnrate() < (2*180/M_PI))
      { // finished first drive and turnrate is zero'ish
        state = 12;
        // wait further 30ms - about one camera frame at 30 FPS
        usleep(35000);
        // start aruco analysis 
        printf("# started new ArUco analysis\n");
        cam->arUcos->setNewFlagToFalse();
        cam->doArUcoAnalysis = true;
      }
      break;
    case 12:
      if (not cam->doArUcoAnalysis)
      { // aruco processing finished
        if (cam->arUcos->getMarkerCount(true) > 0)
        { // found a marker - go to marker (any marker)
          state = 30;
          // tell the operator
          printf("# case=%d found marker\n", state);
          system("espeak \"found marker.\" -ven+f4 -s130 -a5 2>/dev/null &"); 
          bridge->send("oled 5 found marker");
        }
        else
        { // turn a bit (more)
          state = 20;
        }
      }
      break;
    case 20: 
      { // turn a bit and then look for a marker again
        int line = 0;
        snprintf(lines[line++], MAX_LEN, "vel=0.25, tr=0.15: turn=10,time=10");
        snprintf(lines[line++], MAX_LEN, "vel=0,event=2:dist=1");
        sendAndActivateSnippet(lines, line);
        // make sure event 2 is cleared
        bridge->event->isEventSet(2);
        // tell the operator
        printf("# case=%d sent mission turn a bit\n", state);
        system("espeak \"turn.\" -ven+f4 -s130 -a5 2>/dev/null &"); 
        bridge->send("oled 5 code turn a bit");
        state = 21;
        break;
      }
    case 21: // wait until manoeuvre has finished
      if (bridge->event->isEventSet(2))
      {// repeat looking (until all 360 degrees are tested)
        if (featureCnt < 36)
          state = 11;
        else
          state = 999;
        featureCnt++;
      }
      break;
    case 30:
      { // found marker
        // if stop marker, then exit
        ArUcoVal * v = cam->arUcos->getID(6);
        if (v != NULL and v->isNew)
        { // sign to stop
          state = 999;
          break;
        }
        // use the first (assumed only one)
        v = cam->arUcos->getFirstNew();
        v->lock.lock();
        // marker position in robot coordinates
        float xm = v->markerPosition.at<float>(0,0);
        float ym = v->markerPosition.at<float>(0,1);
        float hm = v->markerAngle;
        // stop some distance in front of marker
        float dx = 0.3; // distance to stop in front of marker
        float dy = 0.0; // distance to the left of marker
        xm += - dx*cos(hm) + dy*sin(hm);
        ym += - dx*sin(hm) - dy*cos(hm);
        // limits
        float acc = 1.0; // max allowed acceleration - linear and turn
        float vel = 0.3; // desired velocity
        // set parameters
        // end at 0 m/s velocity
        UPose2pose pp4(xm, ym, hm, 0.0);
        printf("\n");
        // calculate turn-straight-turn (Angle-Line-Angle)  manoeuvre
        bool isOK = pp4.calculateALA(vel, acc);
        // use only if distance to destination is more than 3cm
        if (isOK and (pp4.movementDistance() > 0.03))
        { // a solution is found - and more that 3cm away.
          // debug print manoeuvre details
          pp4.printMan();
          printf("\n");
          // debug end
          int line = 0;
          if (pp4.initialBreak > 0.01)
          { // there is a starting straight part
            snprintf(lines[line++], MAX_LEN, "vel=%.3f,acc=%.1f :dist=%.3f", 
                     pp4.straightVel, acc, pp4.straightVel);
          }
          snprintf(lines[line++], MAX_LEN,   "vel=%.3f,tr=%.3f :turn=%.1f", 
                   pp4.straightVel, pp4.radius1, 
.turnArc1 * 180 / M_PI);
          snprintf(lines[line++], MAX_LEN,   ":dist=%.3f", pp4.straightDist);
          snprintf(lines[line++], MAX_LEN,   "tr=%.3f :turn=%.1f", 
                   pp4.radius2, pp4.turnArc2 * 180 / M_PI);
          if (pp4.finalBreak > 0.01)
          { // there is a straight break distance
            snprintf(lines[line++], MAX_LEN,   "vel=0 : time=%.2f", 
                     sqrt(2*pp4.finalBreak));
          }
          snprintf(lines[line++], MAX_LEN,   "vel=0, event=2: dist=1");
          sendAndActivateSnippet(lines, line);
          // make sure event 2 is cleared
          bridge->event->isEventSet(2);
          //
          // debug
          for (int i = 0; i < line; i++)
          { // print sent lines
            printf("# line %d: %s\n", i, lines[i]);
          }
          // debug end
          // tell the operator
          printf("# Sent mission snippet to marker (%d lines)\n", line);
          //system("espeak \"code snippet to marker.\" -ven+f4 -s130 -a20 2>/dev/null &"); 
          bridge->send("oled 5 code to marker");
          // wait for movement to finish
          state = 31;
        }
        else
        { // no marker or already there
          printf("# No need to move, just %.2fm, frame %d\n", 
                 pp4.movementDistance(), v->frameNumber);
          // look again for marker
          state = 11;
        }
        v->lock.unlock();
      }
      break;
    case 31:
      // wait for event 2 (send when finished driving)
      if (bridge->event->isEventSet(2))
      { // look for next marker
        state = 11;
        // no, stop
        state = 999;
      }
      break;
    case 999:
    default:
      printf("mission 1 ended \n");
      bridge->send("oled 5 \"mission 1 ended.\"");
      finished = true;
      play.stopPlaying();
      break;
  }
  // printf("# mission1 return (state=%d, finished=%d, )\n", state, finished);
  return finished;
}*/



/**
 * Run mission
 * \param state is kept by caller, but is changed here
 *              therefore defined as reference with the '&'.
 *              State will be 0 at first call.
 * \returns true, when finished. */
bool UMission::mission1(int & state)
{
  bool finished = false;
  switch (state)
  {
    case 0:
      {   
        int line = 0;
        bridge->event->isEventSet(distanceCount);

        snprintf(lines[line++], MAX_LEN, "vel=0, acc=0, log=5, white=1, edgel=0: time=1");
	snprintf(lines[line++], MAX_LEN, "servo=3, pservo=900: time=2");
        snprintf(lines[line++], MAX_LEN, "vel=0.2, acc=2, white=1, edgel=0: xl>16");
	snprintf(lines[line++], MAX_LEN, "vel=0.2, acc=2, white=1, edgel=0: dist=0.5");
        snprintf(lines[line++], MAX_LEN, "vel=0.0, acc=2: time=0.2");
        snprintf(lines[line++], MAX_LEN, "vel=0.2, acc=2, tr=0.0: turn=-100");
        snprintf(lines[line++], MAX_LEN, "vel=0.2, acc=2, tr=0.0: turn=5");
	snprintf(lines[line++], MAX_LEN, "vel=-0.2, acc=2: lv>16");
	snprintf(lines[line++], MAX_LEN, "vel=0.0, event=%d: time=1.0", distanceCount);
        sendAndActivateSnippet(lines, line);

        state = 10;
      }
      break;
   	 
    case 10:
      //if (fabsf(bridge->motor->getVelocity()) < 0.001)
      if (bridge->event->isEventSet(distanceCount))
      {
        cam->doObjectDetection = true;

	printf("State 10\n");
	state = 11;
      }
      break;

    case 11:
      if (not cam->doObjectDetection)
      { 
        if (cam->distanceToObject > 0.0 and cam->distanceToObject < 1100.0)
        {
	  printf("State 11, object detected!!!\n");
	  state = 30;
        }
        else
        {
	  printf("State 11, no object detected\n");
          state = 20;
        }
      }
      else
      {
	state = 11;
      }
      break;

    case 20: 
      {
        int line = 0;
	bridge->event->isEventSet(distanceCount);
        snprintf(lines[line++], MAX_LEN, "vel=0.2, acc=2, tr=0.0: turn=90");
	snprintf(lines[line++], MAX_LEN, "vel=0.2, acc=2, tr=0.0: turn=-5");
	snprintf(lines[line++], MAX_LEN, "vel=0.2, acc=2, white=1, edgel=0: dist=0.3");
        snprintf(lines[line++], MAX_LEN, "vel=0.0: time=0.1");
	snprintf(lines[line++], MAX_LEN, "vel=0.2, acc=2, tr=0.0: turn=-90");
	snprintf(lines[line++], MAX_LEN, "vel=0.2, acc=2, tr=0.0: turn=5");
	snprintf(lines[line++], MAX_LEN, "vel=-0.2, acc=2: lv>16");
	snprintf(lines[line++], MAX_LEN, "vel=0.0, event=%d: time=0.1", distanceCount);
        sendAndActivateSnippet(lines, line);
 
        if (distanceCount < 3)
	{
          distanceCount++;
          state = 10;
	  printf("State 20, distanceCount: %d\n",distanceCount);
	}
        else
	{
          state = 66;
	  printf("State 20, max distanceCount reached\n");
	}
      }
      break;

    case 30: 
      {
        int line = 0;
        bridge->event->isEventSet(1);
        dist = ((cam->distanceToObject)/1000-0.20);
        angle = cam->angleToObject;
        printf("The distance result is: %f\n", dist);
	printf("The angle result is: %f\n", angle);

        snprintf(lines[line++], MAX_LEN,   "vel=0.2,acc=2,tr=0.0:turn=%.1f", (1.0)*angle);	
        snprintf(lines[line++], MAX_LEN, "vel=0.2,acc=2 :dist=%.3f", dist);
        snprintf(lines[line++], MAX_LEN,   "vel=0, event=1:time=0.1");
        sendAndActivateSnippet(lines, line);
      
	printf("State 30\n");
        state = 40;
      }
      break;

    case 40: 
      if (bridge->event->isEventSet(1))
      {
        int line = 0;
        bridge->event->isEventSet(1);

        snprintf(lines[line++], MAX_LEN,"servo=3,pservo=-100:time=2");
	snprintf(lines[line++], MAX_LEN,   "vel=0.2,acc=2,tr=0.0 :turn=%.1f", (-1.0)*angle);	
	snprintf(lines[line++], MAX_LEN, "vel=-0.2,acc=2 :lv>16");
	snprintf(lines[line++], MAX_LEN,   "vel=0, event=1:time=0.1");
        sendAndActivateSnippet(lines, line);
      
	printf("State 40\n");
        state = 41;
      }
      break;

    case 41: 
      if (bridge->event->isEventSet(1))
      {
        int line = 0;
        bridge->event->isEventSet(1);
	snprintf(lines[line++], MAX_LEN, "vel=0.2, tr=0.0: turn=90");
	snprintf(lines[line++], MAX_LEN, "vel=0.2, tr=0.0: turn=-5");
	snprintf(lines[line++], MAX_LEN, "vel=0.2, acc=2, white=1, edgel=0: xl>16");
	snprintf(lines[line++], MAX_LEN, "vel=0:time=0.1");
	snprintf(lines[line++], MAX_LEN, "vel=0.2, tr=0.0: turn=90");
	snprintf(lines[line++], MAX_LEN, "vel=0:time=0.1");
	snprintf(lines[line++], MAX_LEN, "vel=0.2, acc=2, white=1, edgel=0:dist=0.1");
	snprintf(lines[line++], MAX_LEN, "vel=0:time=0.1");
	snprintf(lines[line++], MAX_LEN,"servo=3,pservo=900:time=2");
        sendAndActivateSnippet(lines, line);

	printf("State 41\n");
        state = 66;
      }
      break;

    case 66:
      if (bridge->event->isEventSet(1))
      {
        int line = 0;
	bridge->event->isEventSet(1);
        snprintf(lines[line++], MAX_LEN, "vel=0.2, acc=2, tr=0.0: turn=180, time=2");
	snprintf(lines[line++], MAX_LEN, "vel=0.2, acc=2, tr=0.0: turn=-5, time=2");
	snprintf(lines[line++], MAX_LEN, "vel=0.2, acc=2, white=1, edgel=0: xl>16");
        snprintf(lines[line++], MAX_LEN, "vel=0.0: time=0.1");
        sendAndActivateSnippet(lines, line);
        printf("Finishing task\n");       
        state = 999;
      }
      break;
    case 999:
    default:
      printf("mission 1 ended\n");
      bridge->send("oled 5 mission 3 ended\n");
      finished = true;
      break;
  }
  return finished;
}




/**
 * Run mission
 * \param state is kept by caller, but is changed here
 *              therefore defined as reference with the '&'.
 *              State will be 0 at first call.
 * \returns true, when finished. */
bool UMission::mission4(int & state)
{
  bool finished = false;
  switch (state)
  {
    case 999:
    default:
      printf("mission 4 ended\n");
      bridge->send("oled 5 mission 4 ended.");
      finished = true;
      break;
  }
  return finished;
}


void UMission::openLog()
{
  // make logfile
  const int MNL = 100;
  char date[MNL];
  char name[MNL];
  UTime appTime;
  appTime.now();
  appTime.getForFilename(date);
  // construct filename ArUco
  snprintf(name, MNL, "log_mission_%s.txt", date);
  logMission = fopen(name, "w");
  if (logMission != NULL)
  {
    const int MSL = 50;
    char s[MSL];
    fprintf(logMission, "%% Mission log started at %s\n", appTime.getDateTimeAsString(s));
    fprintf(logMission, "%% Start mission %d end mission %d\n", fromMission, toMission);
    fprintf(logMission, "%% 1  Time [sec]\n");
    fprintf(logMission, "%% 2  mission number.\n");
    fprintf(logMission, "%% 3  mission state.\n");
  }
  else
    printf("#UCamera:: Failed to open image logfile\n");
}

void UMission::closeLog()
{
  if (logMission != NULL)
  {
    fclose(logMission);
    logMission = NULL;
  }
}
