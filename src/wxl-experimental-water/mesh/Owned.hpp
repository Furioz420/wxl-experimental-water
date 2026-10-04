// Liquid geometry this module owns: its own buffer pair per chunk group, and its own descriptor.
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
//
// WHY THIS EXISTS. The refinement used to live in the provider's own buffers, described by the
// provider's own cached descriptor, kept alive by the provider's own validity flags. Every one of
// those writes was a statement about the client's state, and the client reads exactly those fields
// to decide what work it can skip -- so a state this module invented sent it down branches written
// for a state it had reached by itself. It skipped filling a descriptor; the caller's stack was
// drawn as a batch; the device divided by zero. Guards were added at three seams and the fourth was
// already waiting, because the provider's buffer pair is BORROWED: allocation walks a free list
// keyed on byte size and hands the same pair to a different group later.
//
// So the geometry is ours instead. Same allocator, our own sizes -- which form their own size
// classes in that free list, so nothing is taken from under anyone -- held per group and released
// at the two moments a group lets go of geometry. The provider keeps a state consistent with
// itself, this module keeps one consistent with itself, and the only invariant left to hold is
// that the descriptor handed to a draw describes the buffers handed to the same draw.

#pragma once

#include "offsets/engine/Liquid.hpp"

namespace wxl::water
{
    /// One chunk group's geometry as this module built it.
    struct OwnedGeometry
    {
        void* chunkBuf = nullptr;   ///< ours, from the engine's own pool allocator
        void* vb       = nullptr;   ///< its vertex view, handed to the draw in place of the stock one
        void* ib       = nullptr;   ///< its index view
        int   vertexCap = 0;        ///< what the pair was sized for, not what is written in it
        int   indexCap  = 0;

        /// The batch that describes what is currently written. Meaningless until `described`,
        /// which is what tells a cache-served draw whether there is a refinement to re-present.
        wxl::offsets::engine::liquid::BatchDesc desc{};
        bool described = false;
    };

    /// The group's geometry, sized for at least this many vertices and indices and created or
    /// regrown to fit. Null when no buffers could be had -- the caller then leaves the provider's
    /// own geometry alone and the group draws stock water, which is always a legal answer.
    OwnedGeometry* AcquireGeometry(void* geom, int vertexCount, int indexCount);

    /// The group's geometry if it already has one. Never creates.
    OwnedGeometry* FindGeometry(void* geom);

    /// Records the memory tag the client stamps its own liquid buffers with, so ours are allocated
    /// the same way. Called from the allocation seam.
    void NoteAllocTag(uint32_t tag);

    /// Groups currently holding a pair of ours, and pairs allocated since boot. Reported with the
    /// rest of the water figures for one reason: the records are released by detours on the client's
    /// own release path, and if those ever stopped firing the only visible symptom would be memory
    /// quietly running out. A live count that tracks what is on screen says they fire; one that only
    /// climbs says they do not.
    int OwnedCount();
    int OwnedAllocations();

    /// Detours the two seams where a group releases geometry (Purge and the factory destructor).
    /// Without these a record outlives its group and its buffers are never returned.
    bool InstallGeometryOwnership();
}
