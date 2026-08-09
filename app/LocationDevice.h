//
//  LocationDevice.h
//  iSH
//
//  Created by Theodore Dubois on 10/20/19.
//

// const, matching the definition in LocationDevice.m and the convention in
// AudioDevice.h. The mismatch went unnoticed because this header was never
// imported by its own implementation file.
extern const struct dev_ops location_dev;

// True when background location updates are actually running, which is what
// keeps the app alive in the background (allowsBackgroundLocationUpdates plus
// startUpdatingLocation). Both conditions are needed: the tracker is created
// lazily when the guest opens the location device, and updates only start when
// location is authorized. Callers use this to tell "we are backgrounded and
// heading for suspension" apart from "we are backgrounded and legitimately
// still running", which decides whether the fakefs should be quiesced. Does not
// create the tracker.
bool ISHLocationKeepsAppAlive(void);
