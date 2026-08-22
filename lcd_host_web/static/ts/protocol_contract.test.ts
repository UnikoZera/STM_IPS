import { describe, expect, it } from 'vitest';
import { PROTOCOL } from '../js/protocol_contract.js';

describe('protocol contract', () => {
  it('keeps host frame and command identifiers stable', () => {
    expect(PROTOCOL.version).toBe(1);
    expect(PROTOCOL.hostHeader).toEqual([0xBB, 0x44]);
    expect(PROTOCOL.hostMaxDataBytes).toBe(1024);
    expect(PROTOCOL.commands.continue).toBe(0xA1);
  });
});
