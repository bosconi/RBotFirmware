#pragma once

#include "MotionPipeline.h"
typedef bool(*ptToActuatorFnType) (MotionPipelineElem& motionElem, AxisFloats& actuatorCoords, AxisParams axisParams[], int numAxes);
typedef void(*actuatorToPtFnType) (AxisFloats& actuatorCoords, AxisFloats& xy, AxisParams axisParams[], int numAxes);
typedef void(*correctStepOverflowFnType) (AxisParams axisParams[], int numAxes);

class MotionPlanner
{
public:
	static constexpr double MINIMUM_MOVE_DIST_MM = 0.0001;

private:
	// Previous pipeline element
	bool _prevPipelineElemValid;
	MotionPipelineElem _prevPipelineElem;
	// Minimum planner speed mm/s
	float _minimumPlannerSpeedMMps;
	// Junction deviation
	float _junctionDeviation;
	// Motion parameters shared by many calculations
	MotionPipelineElem::motionParams _motionParams;

public:
	MotionPlanner()
	{
		_prevPipelineElemValid = false;
		_minimumPlannerSpeedMMps = 0;
		// Configure the motion pipeline - these values will be changed in config
		_junctionDeviation = 0;
	}

	void configure(float junctionDeviation)
	{
		_junctionDeviation = junctionDeviation;
	}

	bool moveTo(MotionPipelineElem& elem, AxisPosition& curAxisPositions,
				AxesParams& axesParams, MotionPipeline& motionPipeline)
	{
		// Motion parameters used throughout planner process
		_motionParams._accMMps2 = axesParams.getMasterMaxAccel();

		// Find axis deltas and sum of squares of motion on primary axes
		double deltas[RobotConsts::MAX_AXES];
		bool isAMove = false;
		bool isAPrimaryMove = false;
		double squareSum = 0;
		for (int axisIdx = 0; axisIdx < RobotConsts::MAX_AXES; axisIdx++)
		{
			deltas[axisIdx] = elem._pt2MM._pt[axisIdx] - curAxisPositions._axisPositionMM._pt[axisIdx];
			if (deltas[axisIdx] != 0)
			{
				isAMove = true;
				if (axesParams.isPrimaryAxis(axisIdx))
				{
					squareSum += pow(deltas[axisIdx], 2);
					isAPrimaryMove = true;
				}
			}
		}

		// Distance being moved
		double moveDist = sqrt(squareSum);

		// Ignore if there is no real movement
		if (!isAMove || moveDist < MINIMUM_MOVE_DIST_MM)
			return false;

		Log.trace("Moving %0.3f mm", moveDist);

		// Find the unit vectors
		AxisFloats unitVec;
		double maxSpeedMMps = elem._maxSpeedMMps;
		for (int axisIdx = 0; axisIdx < RobotConsts::MAX_AXES; axisIdx++)
		{
			if (axesParams.isPrimaryAxis(axisIdx))
			{
				unitVec._pt[axisIdx] = float(deltas[axisIdx] / moveDist);
				// Check that the move speed doesn't exceed max
				if (axesParams.getMaxSpeed(axisIdx) > 0)
				{
					if (maxSpeedMMps > 0)
					{
						double axisSpeed = fabs(unitVec._pt[axisIdx] * maxSpeedMMps);
						if (axisSpeed > axesParams.getMaxSpeed(axisIdx))
						{
							maxSpeedMMps *= axisSpeed / axesParams.getMaxSpeed(axisIdx);
						}
					}
					else
					{
						maxSpeedMMps = axesParams.getMaxSpeed(axisIdx);
					}
				}
			}
		}

		// Calculate move time
		double reciprocalTime = maxSpeedMMps / moveDist;
		Log.trace("Feedrate %0.3f, reciprocalTime %0.3f", maxSpeedMMps, reciprocalTime);

		// Check speed limits for each axis individually
		for (int axisIdx = 0; axisIdx < RobotConsts::MAX_AXES; axisIdx++)
		{
			// Speed and time
			double axisDist = fabs(elem._pt2MM._pt[axisIdx] - curAxisPositions._axisPositionMM._pt[axisIdx]);
			if (axisDist == 0)
				continue;
			double axisReqdAcc = axisDist * reciprocalTime;
			if (axisReqdAcc > axesParams.getMaxAccel(axisIdx))
			{
				maxSpeedMMps *= axesParams.getMaxAccel(axisIdx) / axisReqdAcc;
				reciprocalTime = maxSpeedMMps / moveDist;
			}
		}

		Log.trace("Feedrate %0.3f, reciprocalTime %0.3f", maxSpeedMMps, reciprocalTime);


		Log.trace("MotionPipelineElem delta %0.3f", elem.delta());

		// Store values in the block
		elem._maxSpeedMMps = float(maxSpeedMMps);
		elem._primaryAxisMove = isAPrimaryMove;
		elem._unitVec = unitVec;
		elem._moveDistMM = float(moveDist);

		// Add the block to the planner queue
		return addBlock(motionPipeline, elem, curAxisPositions);
	}

	bool addBlock(MotionPipelineElem& elem, float moveDist, float *unitVec)
	{
		bool hasSteps = false;
		return true;
	}

	bool addBlock(MotionPipeline& motionPipeline, MotionPipelineElem& elem, AxisPosition& curAxisPosition)
	{
		// Find if there are any steps
		bool hasSteps = false;
		uint32_t axisMaxSteps = 0;
		for (int axisIdx = 0; axisIdx < RobotConsts::MAX_AXES; axisIdx++)
		{
			// Check if any actual steps to perform
			int steps = int(std::ceil(elem._destActuatorCoords._pt[axisIdx] - curAxisPosition._stepsFromHome._pt[axisIdx]));
			if (steps != 0)
				hasSteps = true;
			// Direction
			elem._stepDirn.setVal(axisIdx, steps < 0);
			elem._absSteps.vals[axisIdx] = labs(steps);
			if (axisMaxSteps < elem._absSteps.vals[axisIdx])
				axisMaxSteps = elem._absSteps.vals[axisIdx];
		}

		// Check there are some actual steps
		if (!hasSteps)
			return false;

		// Max steps for an axis
		elem._axisMaxSteps = axisMaxSteps;

		// Check movement distance
		if (elem._moveDistMM > 0.0f)
		{
			elem._nominalSpeedMMps = elem._maxSpeedMMps;
			elem._nominalStepRatePerSec = axisMaxSteps * elem._maxSpeedMMps / elem._moveDistMM;
		}
		else
		{
			elem._nominalSpeedMMps = 0;
			elem._nominalStepRatePerSec = 0;
		}

		Log.trace("maxSteps %lu, nomSpeed %0.3f mm/s, nomRate %0.3f steps/s",
			axisMaxSteps, elem._nominalSpeedMMps, elem._nominalStepRatePerSec);

		// Comments from Smoothieware!
		// Compute the acceleration rate for the trapezoid generator. Depending on the slope of the line
		// average travel per step event changes. For a line along one axis the travel per step event
		// is equal to the travel/step in the particular axis. For a 45 degree line the steppers of both
		// axes might step for every step event. Travel per step event is then sqrt(travel_x^2+travel_y^2).

		// Compute maximum allowable entry speed at junction by centripetal acceleration approximation.
		// Let a circle be tangent to both previous and current path line segments, where the junction
		// deviation is defined as the distance from the junction to the closest edge of the circle,
		// colinear with the circle center. The circular segment joining the two paths represents the
		// path of centripetal acceleration. Solve for max velocity based on max acceleration about the
		// radius of the circle, defined indirectly by junction deviation. This may be also viewed as
		// path width or max_jerk in the previous grbl version. This approach does not actually deviate
		// from path, but used as a robust way to compute cornering speeds, as it takes into account the
		// nonlinearities of both the junction angle and junction velocity.

		// NOTE however it does not take into account independent axis, in most cartesian X and Y and Z are totally independent
		// and this allows one to stop with little to no decleration in many cases. This is particualrly bad on leadscrew based systems that will skip steps.

		// Junction deviation
		float junctionDeviation = _junctionDeviation;
		float vmaxJunction = _minimumPlannerSpeedMMps;

		// Invalidate the prev element if the pipeline becomes empty
		if (!motionPipeline.canGet())
			_prevPipelineElemValid = false;

		// For primary axis moves compute the vmaxJunction
		// But only if there are still elements in the queue
		// If not assume the robot is idle and start the move
		// from scratch
		if (elem._primaryAxisMove && _prevPipelineElemValid)
		{
			float prevNominalSpeed = _prevPipelineElem._primaryAxisMove ? _prevPipelineElem._nominalSpeedMMps : 0;
			if (junctionDeviation > 0.0f && prevNominalSpeed > 0.0f)
			{
				// Compute cosine of angle between previous and current path. (prev_unit_vec is negative)
				// NOTE: Max junction velocity is computed without sin() or acos() by trig half angle identity.
				float cosTheta = -_prevPipelineElem._unitVec.X() * elem._unitVec.X()
					- _prevPipelineElem._unitVec.Y() * elem._unitVec.Y()
					- _prevPipelineElem._unitVec.Z() * elem._unitVec.Z();

				// Skip and use default max junction speed for 0 degree acute junction.
				if (cosTheta < 0.95F) {
					vmaxJunction = std::min(prevNominalSpeed, elem._nominalSpeedMMps);
					// Skip and avoid divide by zero for straight junctions at 180 degrees. Limit to min() of nominal speeds.
					if (cosTheta > -0.95F) {
						// Compute maximum junction velocity based on maximum acceleration and junction deviation
						float sinThetaD2 = sqrtf(0.5F * (1.0F - cosTheta)); // Trig half angle identity. Always positive.
						vmaxJunction = std::min(vmaxJunction, sqrtf(_motionParams._accMMps2 * junctionDeviation * sinThetaD2 / (1.0F - sinThetaD2)));
					}
				}
			}
		}
		elem._maxEntrySpeedMMps = vmaxJunction;

		Log.trace("PrevMoveInQueue %d, JunctionDeviation %0.3f, VmaxJunction %0.3f", motionPipeline.canGet(), junctionDeviation, vmaxJunction);

		// Calculate max allowable speed using v^2 = u^2 - 2as
		// Was acceleration*60*60*distance, in case this breaks, but here we prefer to use seconds instead of minutes
		float maxAllowableSpeedMMps = sqrtf(_minimumPlannerSpeedMMps * _minimumPlannerSpeedMMps - 2.0F * (-_motionParams._accMMps2) * elem._moveDistMM);
		elem._entrySpeedMMps = std::min(vmaxJunction, maxAllowableSpeedMMps);

		// Initialize planner efficiency flags
		// Set flag if block will always reach maximum junction speed regardless of entry/exit speeds.
		// If a block can de/ac-celerate from nominal speed to zero within the length of the block, then
		// the current block and next block junction speeds are guaranteed to always be at their maximum
		// junction speeds in deceleration and acceleration, respectively. This is due to how the current
		// block nominal speed limits both the current and next maximum junction speeds. Hence, in both
		// the reverse and forward planners, the corresponding block junction speed will always be at the
		// the maximum junction speed and may always be ignored for any speed reduction checks.
		elem._nominalLengthFlag = (elem._nominalSpeedMMps <= maxAllowableSpeedMMps);

		Log.trace("MaxAllowableSpeed %0.3f, entrySpeedMMps %0.3f, nominalLengthFlag %d", maxAllowableSpeedMMps, elem._entrySpeedMMps, elem._nominalLengthFlag);

		// Recalculation required
		elem._recalcFlag = true;

		// Store the element in the queue and remember previous element
		motionPipeline.add(elem);
		_prevPipelineElem = elem;
		_prevPipelineElemValid = true;

		// Recalculate the whole queue
		recalculatePipeline(motionPipeline);

		return true;
	}

	void recalculatePipeline(MotionPipeline& motionPipeline)
	{
		// A newly added block is decel limited
		// We find its max entry speed given its exit speed
		//
		// For each block, walking backwards in the queue :
		//    if max entry speed == current entry speed
		//    then we can set recalculate to false, since clearly adding another block didn't allow us to enter faster
		//    and thus we don't need to check entry speed for this block any more
		//	
		// Once we find an accel limited block, we must find the max exit speed and walk the queue forwards
		//	
		// For each block, walking forwards in the queue :
		//
		//    given the exit speed of the previous block and our own max entry speed
		//    we can tell if we're accel or decel limited (or coasting)
		//	
		//    if prev_exit > max_entry
		//    then we're still decel limited. update previous trapezoid with our max entry for prev exit
		//    if max_entry >= prev_exit
		//    then we're accel limited. set recalculate to false, work out max exit speed

		Log.trace("------recalculatePipeline");
		// Step 1:
		// For each block, given the exit speed and acceleration, find the maximum entry speed
		float entrySpeed = _minimumPlannerSpeedMMps;
		int elemIdxFromPutPos = 0;
		MotionPipelineElem* pElem = NULL;
		MotionPipelineElem* pPrevElem = NULL;
		MotionPipelineElem* pNext = NULL;
		if (motionPipeline.peekNthFromPut(elemIdxFromPutPos) != NULL)
		{
			pElem = motionPipeline.peekNthFromPut(elemIdxFromPutPos);
			while (pElem && pElem->_recalcFlag)
			{
				// Get the max entry speed
				pElem->calcMaxSpeedReverse(entrySpeed, _motionParams);
				Log.trace("Backwards pass #%d element %08x, tryEntrySpeed %0.3f, newEntrySpeed %0.3f", elemIdxFromPutPos, pElem, entrySpeed, pElem->_entrySpeedMMps);
				entrySpeed = pElem->_entrySpeedMMps;
				// Next elem
				pNext = motionPipeline.peekNthFromPut(elemIdxFromPutPos + 1);
				if (!pNext)
					break;
				pElem = pNext;
				// The exit speed for the previous block up the chain is the entry speed we've just calculated
				pElem->_exitSpeedMMps = entrySpeed;
				elemIdxFromPutPos++;
			}

			// Calc exit speed of first block
			pElem->maximizeExitSpeed(_motionParams);

			// Step 2:
			// Pointing to the first block that doesn't need recalculating (or the get block)
			while (true)
			{
				// Save previous
				pPrevElem = pElem;
				// Next element
				if (elemIdxFromPutPos == 0)
				{
					// Calculate trapezoid on last element
					pPrevElem->calculateTrapezoid(_motionParams);
					Log.trace("Forward pass cur #%d after trapezoid entrySpeed %0.3f, exitSpeed %0.3f", elemIdxFromPutPos, pPrevElem->_entrySpeedMMps, pPrevElem->_exitSpeedMMps);
					break;
				}
				elemIdxFromPutPos--;
				pElem = motionPipeline.peekNthFromPut(elemIdxFromPutPos);
				if (!pElem)
					break;
				// Pass the exit speed of the previous block
				// so this block can decide if it's accel or decel limited and update its fields as appropriate
				pElem->calcMaxSpeedForward(pPrevElem->_exitSpeedMMps, _motionParams);
				Log.trace("Forward pass #%d element %08x, prev En %0.3f Ex %0.3f. cur En %0.3f Ex %0.3f", elemIdxFromPutPos, pElem,
					pPrevElem->_entrySpeedMMps, pPrevElem->_exitSpeedMMps, pElem->_entrySpeedMMps, pElem->_exitSpeedMMps);

				// Set exit speed and calculate trapezoid on this block
				pPrevElem->calculateTrapezoid(_motionParams);
				Log.trace("Forward pass #%d after trapezoid entrySpeed %0.3f, exitSpeed %0.3f", elemIdxFromPutPos+1, pPrevElem->_entrySpeedMMps, pPrevElem->_exitSpeedMMps);
			}

		}
	}
};