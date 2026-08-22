# STM IPS Host Protocol Contract

`contract.json` is the host-side canonical description of the USB CDC frame
layout and command identifiers. It mirrors the constants in
`Core/Src/storage_manager.c`; changes to either side must update the contract
and its golden-vector tests together.

Host frames use `BB 44` and a 9-byte header. `payload_length` includes the
two-byte little-endian CRC. CRC-16/USB covers only the data payload, not the
command, size fields, or CRC itself. An empty payload uses `0000` as required
by the current firmware compatibility path.

Device frames use `AA 55` and a 5-byte header. The parser must resynchronise
on the next magic sequence and reject payloads larger than 32768 bytes.

The contract version must be incremented when frame layout or command
semantics change. Add a golden vector before changing firmware or host code.
