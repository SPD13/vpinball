# PinMAME memory maps

Maps of the memory of the machines emulated by PinMAME, from the Pinball Memory Maps project:
https://github.com/tomlogic/pinmame-nvram-maps (revision 7e63610464453e1902d6de2f90529a0705fdc2a2).

They are made available under the Open Database License (ODbL) v1.0 (see LICENSE-ODbL.md), the content of the
maps under the Database Contents License (DbCL) v1.0 (see LICENSE-DbCL). Copyright (C) Tom Collins and the
contributors of the project.

The PinMAME plugin loads the map of the running ROM when no map is found along the table or in the PinMAME folder,
and exposes its 'game_state' and 'high_scores' entries as controller states, from which Visual Pinball records the
scores of the games (leaderboards). Only index.json, maps/ and platforms/ are copied from the project.
