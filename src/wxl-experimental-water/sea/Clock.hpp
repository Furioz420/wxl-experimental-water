// The clock the sea runs on, and why it is not the frame timer.
// Copyright (C) 2026 WarcraftXL
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#pragma once

// The whole sea is a function of (spectrum, time). Two clients that agree on the time compute the
// same water down to the last crest -- which is what lets a wave lift a boat without anyone
// replicating a height field. The clock is therefore the one input that has to be shared, and it is
// the piece the surface cannot be correct without.
//
// AND IT IS CURRENTLY THE SESSION'S OWN START, which is correct for one player and wrong for every
// other. A local origin means two players who logged in an hour apart are an hour out of phase and
// see different water standing in the same place. Nothing else in the sea has to change for that to
// be fixed -- the surface is already a closed form of (spectrum, time) -- so the whole of the
// multiplayer work is giving this one function an origin both ends agree on.
namespace wxl::water
{
    /// Unscaled elapsed seconds, for weather response and other real-time intervals.
    double SessionSeconds();

    /// Water animation seconds, at 75% of the original rate. CPU waves, FFT detail and shader
    /// motion share this clock so the surface and its shading stay in phase.
    /// Double because a float loses sub-frame resolution within
    /// a day of uptime, and phase is exactly what that resolution is spent on.
    double Now();
}
