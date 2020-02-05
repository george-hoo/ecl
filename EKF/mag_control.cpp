/****************************************************************************
 *
 *   Copyright (c) 2019 Estimation and Control Library (ECL). All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name ECL nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * @file mag_control.cpp
 * Control functions for ekf magnetic field fusion
 */

#include "ekf.h"
#include <mathlib/mathlib.h>

void Ekf::controlMagFusion()
{
	updateMagFilter();
	checkMagFieldStrength();
	processMagResetFlags();

	// If we are on ground, store the local position and time to use as a reference
	// Also reset the flight alignment flag so that the mag fields will be re-initialised next time we achieve flight altitude
	if (!_control_status.flags.in_air) {
		_last_on_ground_posD = _state.pos(2);
		_control_status.flags.mag_aligned_in_flight = false;
		_num_bad_flight_yaw_events = 0;
	}

	if ((_params.mag_fusion_type >= MAG_FUSE_TYPE_NONE)
	    || _control_status.flags.mag_fault) {
		// NOTE: we still run the mag fusion logic if the flag _mag_use_inhibit is set because
		// this special case is handled in the fuseHeading function
		stopMagFusion();
		return;
	}

	if (canRunMagFusion()) {
		checkHaglYawResetReq();

		// Determine if we should use simple magnetic heading fusion which works better when
		// there are large external disturbances or the more accurate 3-axis fusion
		switch (_params.mag_fusion_type) {
		case MAG_FUSE_TYPE_AUTO:
			selectMagAuto();
			break;

		case MAG_FUSE_TYPE_INDOOR:
		/* fallthrough */
		case MAG_FUSE_TYPE_HEADING:
			startMagHdgFusion();
			break;

		case MAG_FUSE_TYPE_3D:
			startMag3DFusion();
			break;

		default:
			selectMagAuto();
			break;
		}

		checkMagDeclRequired();
		checkMagInhibition();

		runMagAndMagDeclFusions();
		checkMagBiasConvergence();
	}
}

void Ekf::updateMagFilter()
{
	if (_mag_data_ready) {
		_mag_lpf.update(_mag_sample_delayed.mag);
	}
}

bool Ekf::canRunMagFusion() const
{
	// check for new magnetometer data that has fallen behind the fusion time horizon
	// If we are using external vision data or GPS-heading for heading then no magnetometer fusion is used
	return !_control_status.flags.ev_yaw && !_control_status.flags.gps_yaw && _mag_data_ready;
}

void Ekf::checkHaglYawResetReq()
{
	// We need to reset the yaw angle after climbing away from the ground to enable
	// recovery from ground level magnetic interference.
	if (!_control_status.flags.mag_aligned_in_flight) {
		// Check if height has increased sufficiently to be away from ground magnetic anomalies
		// and request a yaw reset if not already requested.
		static constexpr float mag_anomalies_max_hagl = 1.5f;
		const bool above_mag_anomalies = (getTerrainVPos() - _state.pos(2)) > mag_anomalies_max_hagl;
		_in_flight_yaw_reset_req = above_mag_anomalies;
	}
}

float Ekf::getTerrainVPos() const
{
	return isTerrainEstimateValid() ? _terrain_vpos : _last_on_ground_posD;
}

bool Ekf::isYawResetAuthorized() const
{
	return !_mag_use_inhibit && _control_status.flags.tilt_align;
}

bool Ekf::canResetMagHeading() const
{
	return !isStrongMagneticDisturbance();
}

bool Ekf::canRealignYawUsingGps() const
{ 
	return _control_status.flags.fixed_wing && _control_status.flags.in_air;
}

void Ekf::runVelPosReset()
{
	if (_velpos_reset_request) {
		resetVelocity();
		resetPosition();
		_velpos_reset_request = false;
	}
}

void Ekf::selectMagAuto()
{
	check3DMagFusionSuitability();
	canUse3DMagFusion() ? startMag3DFusion() : startMagHdgFusion();
}

void Ekf::check3DMagFusionSuitability()
{
	checkYawAngleObservability();
	checkMagBiasObservability();

	if (isMagBiasObservable() || isYawAngleObservable()) {
		_time_last_mov_3d_mag_suitable = _imu_sample_delayed.time_us;
	}
}

void Ekf::checkYawAngleObservability()
{
	// Check if there has been enough change in horizontal velocity to make yaw observable
	// Apply hysteresis to check to avoid rapid toggling
	_yaw_angle_observable = _yaw_angle_observable
				? _accel_lpf_NE.norm() > _params.mag_acc_gate
				: _accel_lpf_NE.norm() > 2.0f * _params.mag_acc_gate;

	_yaw_angle_observable = _yaw_angle_observable
				&& (_control_status.flags.gps || _control_status.flags.ev_pos); // Do we have to add ev_vel here?
}

void Ekf::checkMagBiasObservability()
{
	// check if there is enough yaw rotation to make the mag bias states observable
	if (!_mag_bias_observable && (fabsf(_yaw_rate_lpf_ef) > _params.mag_yaw_rate_gate)) {
		// initial yaw motion is detected
		_mag_bias_observable = true;

	} else if (_mag_bias_observable) {
		// require sustained yaw motion of 50% the initial yaw rate threshold
		const float yaw_dt = 1e-6f * (float)(_imu_sample_delayed.time_us - _time_yaw_started);
		const float min_yaw_change_req =  0.5f * _params.mag_yaw_rate_gate * yaw_dt;
		_mag_bias_observable = fabsf(_yaw_delta_ef) > min_yaw_change_req;
	}

	_yaw_delta_ef = 0.0f;
	_time_yaw_started = _imu_sample_delayed.time_us;
}

bool Ekf::isYawAngleObservable() const
{
	return _yaw_angle_observable;
}

bool Ekf::isMagBiasObservable() const
{
	return _mag_bias_observable;
}

bool Ekf::canUse3DMagFusion() const
{
	// Use of 3D fusion requires an in-air heading alignment but it should not
	// be used when the heading and mag biases are not observable for more than 2 seconds
	return _control_status.flags.mag_aligned_in_flight
	       && ((_imu_sample_delayed.time_us - _time_last_mov_3d_mag_suitable) < (uint64_t)2e6);
}

void Ekf::checkMagDeclRequired()
{
	// if we are using 3-axis magnetometer fusion, but without external NE aiding,
	// then the declination must be fused as an observation to prevent long term heading drift
	// fusing declination when gps aiding is available is optional, but recommended to prevent
	// problem if the vehicle is static for extended periods of time
	const bool user_selected = (_params.mag_declination_source & MASK_FUSE_DECL);
	const bool not_using_ne_aiding = !_control_status.flags.gps;
	_control_status.flags.mag_dec = (_control_status.flags.mag_3D && (not_using_ne_aiding || user_selected));
}

void Ekf::checkMagInhibition()
{
	_mag_use_inhibit = shouldInhibitMag();
	if (!_mag_use_inhibit) { 
		_mag_use_not_inhibit_us = _imu_sample_delayed.time_us;
	}

	// If magnetometer use has been inhibited continuously then a yaw reset is required for a valid heading
	if (uint32_t(_imu_sample_delayed.time_us - _mag_use_not_inhibit_us) > (uint32_t)5e6) {
		_mag_inhibit_yaw_reset_req = true;
	}
}

bool Ekf::shouldInhibitMag() const
{
	// If the user has selected auto protection against indoor magnetic field errors, only use the magnetometer
	// if a yaw angle relative to true North is required for navigation. If no GPS or other earth frame aiding
	// is available, assume that we are operating indoors and the magnetometer should not be used.
	// Also inhibit mag fusion when a strong magnetic field interference is detected
	const bool user_selected = (_params.mag_fusion_type == MAG_FUSE_TYPE_INDOOR);

	const bool heading_not_required_for_navigation = !_control_status.flags.gps
							 && !_control_status.flags.ev_pos
							 && !_control_status.flags.ev_vel;

	return (user_selected && heading_not_required_for_navigation)
	       || isStrongMagneticDisturbance();
}

void Ekf::checkMagFieldStrength()
{
	if (_params.check_mag_strength) {
		_control_status.flags.mag_field_disturbed = isMagStrengthPlausible(_mag_sample_delayed.mag.length())
							    && isMagStrengthPlausible(_mag_lpf.getState().length());

	} else {
		_control_status.flags.mag_field_disturbed = false;
	}
}

bool Ekf::isMagStrengthPlausible(float mag_strength) const
{
	return _NED_origin_initialised
	       ? !isMeasuredMatchingGpsMagStrength(mag_strength)
	       : !isMeasuredMatchingAverageMagStrength(mag_strength);
}

bool Ekf::isStrongMagneticDisturbance() const
{
	return _control_status.flags.mag_field_disturbed;
}

bool Ekf::isMeasuredMatchingGpsMagStrength(float mag_strength) const
{
	constexpr float wmm_gate_size = 0.2f; // +/- Gauss
	return isMeasuredMatchingExpected(mag_strength, _mag_strength_gps, wmm_gate_size);
}

bool Ekf::isMeasuredMatchingAverageMagStrength(float mag_strength) const
{
	constexpr float average_earth_mag_field_strength = 0.45f; // Gauss
	constexpr float average_earth_mag_gate_size = 0.40f; // +/- Gauss
	return isMeasuredMatchingExpected(mag_strength,
					  average_earth_mag_field_strength,
					  average_earth_mag_gate_size);
}

bool Ekf::isMeasuredMatchingExpected(const float measured, const float expected, const float gate)
{
	return (measured >= expected - gate)
		&& (measured <= expected + gate);
}

void Ekf::processMagResetFlags()
{
	if (isYawResetRequested() && isYawResetAuthorized()) {
		bool has_realigned_yaw = false;

		if (canRealignYawUsingGps()) {
			has_realigned_yaw = realignYawGPS();
			runVelPosReset(); // TODO: move this somewhere else
		}

		if (!has_realigned_yaw && canResetMagHeading()) {
			has_realigned_yaw = resetMagHeading(_mag_lpf.getState(), shouldIncreaseYawVar(), shouldUpdateBuffer());
		}

		if (has_realigned_yaw) {
			runPostAlignmentActions();
		}
	}
}

bool Ekf::isYawResetRequested() const
{
	return _mag_yaw_reset_req ||
	       _mag_inhibit_yaw_reset_req ||
	       _in_flight_yaw_reset_req ||
	        _mag_bias_converged_yaw_reset_req ||
	       !_control_status.flags.yaw_align;
}

bool Ekf::shouldIncreaseYawVar() const
{
	// TODO: We always increase the yaw variance after a yaw reset,
	// do we really want that? If not we can remove the argument.
	return true;
}

bool Ekf::shouldUpdateBuffer() const
{
	return _mag_yaw_reset_req ||
	       _mag_inhibit_yaw_reset_req ||
	       _in_flight_yaw_reset_req ||
	       _mag_bias_converged_yaw_reset_req;
}

void Ekf::runPostAlignmentActions()
{
	_control_status.flags.yaw_align = true;
	_mag_yaw_reset_req = false;

	if (_in_flight_yaw_reset_req) {
		_control_status.flags.mag_aligned_in_flight = true;
		_in_flight_yaw_reset_req = false;
	}

	if (_mag_bias_converged_yaw_reset_req) {
		_control_status.flags.mag_unbiased_align = true;
		_mag_bias_converged_yaw_reset_req = false;
	}

	// Handle the special case where we have not been constraining yaw drift or learning yaw bias due
	// to assumed invalid mag field associated with indoor operation with a downwards looking flow sensor.
	if (_mag_inhibit_yaw_reset_req) {
		// Zero the yaw bias covariance and set the variance to the initial alignment uncertainty
		setDiag(P, 12, 12, sq(_params.switch_on_gyro_bias * FILTER_UPDATE_PERIOD_S));
		_mag_inhibit_yaw_reset_req = false;
	}
}

void Ekf::runMagAndMagDeclFusions()
{
	if (_control_status.flags.mag_3D) {
		run3DMagAndDeclFusions();

	} else if (_control_status.flags.mag_hdg) {
		fuseHeading();
	}
}

void Ekf::run3DMagAndDeclFusions()
{
	if (!_mag_decl_cov_reset) {
		// After any magnetic field covariance reset event the earth field state
		// covariances need to be corrected to incorporate knowledge of the declination
		// before fusing magnetomer data to prevent rapid rotation of the earth field
		// states for the first few observations.
		fuseDeclination(0.02f);
		_mag_decl_cov_reset = true;
		fuseMag();

	} else {
		// The normal sequence is to fuse the magnetometer data first before fusing
		// declination angle at a higher uncertainty to allow some learning of
		// declination angle over time.
		fuseMag();
		if (_control_status.flags.mag_dec) {
			fuseDeclination(0.5f);
		}
	}
}

void Ekf::checkMagBiasConvergence()
{
	if (!_control_status.flags.mag_3D) {
		return;
	}

	if (hasMagBiasConverged()) {
		if (!_control_status.flags.mag_unbiased_align) {
			// We just learned the mag biases, reset to the corrected values
			// in case we have a strong yaw error
			_mag_bias_converged_yaw_reset_req = true;
		}

	} else {
		// The flag need to be cleared if the state uncertenty increased
		// to allow for a new reset when they converge again
		_control_status.flags.mag_unbiased_align = false;
	}
}

bool Ekf::hasMagBiasConverged() const
{
	return (P[19][19] < 2e-4f)
		&& (P[20][20] < 2e-4f)
		&& ((P[19][19] + P[20][20] + P[21][21]) > 1e-10f);

}
