/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

//! Animated types for box properties.

use crate::values::animated::{Animate, Procedure, ToAnimatedZero};
use crate::values::distance::{ComputeSquaredDistance, SquaredDistance};
use crate::values::computed::Display;

/// <https://drafts.csswg.org/css-display-4/#display-animation>
impl Animate for Display {
    #[inline]
    fn animate(&self, other: &Self, procedure: Procedure) -> Result<Self, ()> {
        match procedure {
            Procedure::Interpolate { progress } => {
                let (this_weight, other_weight) = procedure.weights();
                match (*self, *other) {
                    (_, Display::None) => {
                        Ok(if this_weight > 0.0 { *self } else { *other })
                    },
                    (Display::None, _) => {
                        Ok(if other_weight > 0.0 { *other } else { *self })
                    },
                    _ => Ok(if progress >= 0.5 { *other } else { *self }),
                }
            },
            _ => Err(()),
        }
    }
}

impl ComputeSquaredDistance for Display {
    #[inline]
    fn compute_squared_distance(&self, other: &Self) -> Result<SquaredDistance, ()> {
        Ok(SquaredDistance::from_sqrt(if *self == *other { 0. } else { 1. }))
    }
}

impl ToAnimatedZero for Display {
    #[inline]
    fn to_animated_zero(&self) -> Result<Self, ()> {
        Err(())
    }
}

