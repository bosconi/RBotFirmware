// RBotFirmware
// Rob Dobson 2016

#pragma once

#include "RobotCommandArgs.h"
#include "AxisParams.h"

class RobotBase
{
protected:
    String _robotTypeName;

public:
    RobotBase(const char* pRobotTypeName) :
        _robotTypeName(pRobotTypeName)
    {
    }

    ~RobotBase()
    {
    }

    // Pause (or un-pause) all motion
    virtual void pause(bool pauseIt)
    {
    }

    // Check if paused
    virtual bool isPaused()
    {
        return false;
    }

    virtual bool init(const char* robotConfigStr)
    {
        return false;
    }

    virtual bool canAcceptCommand()
    {
        return false;
    }

    virtual void service()
    {
    }

    // Movement commands
    virtual void actuator(double value)
    {
    }

    virtual void moveTo(RobotCommandArgs& args)
    {
    }

    // Homing commands
    virtual void home(RobotCommandArgs& args)
    {
    }

    virtual bool wasActiveInLastNSeconds(int nSeconds)
    {
        return false;
    }

};