#!/usr/bin/env python3
"""
host_kex.py -- POST-QUANTUM KEY EXCHANGE BETWEEN TWO KC705 BOARDS.

Two FPGAs, each running ML-KEM-512, independently arrive at the SAME 32-byte
shared secret. This is the demonstration people actually understand: not a
test vector matching, but two physical devices agreeing on a key.

    ALICE (board A)                          BOB (board B)
    ---------------                          -------------
    KeyGen  -> ek, dk
              ek  ------- public key ------>  (staged into ek buffer)
                                              Encaps -> K_bob, c
              c  <------- ciphertext -------  (read out)
    Decaps -> K_alice

    K_alice == K_bob      <-- both boards print it

The public key and the ciphertext are the ONLY things that cross the link.
An eavesdropper who captures both still cannot derive the secret -- that is
the whole point of a KEM, and it is worth saying out loud during the demo.

The PC here is only a courier. It does not compute any part of the KEM: it
reads ek off Alice, writes it into Bob, reads c off Bob, writes it into
Alice. Both secrets are produced inside the FPGAs.

WIRING
    Two KC705 boards, each with its own USB-UART. Two COM ports.
    No board-to-board wiring needed -- the PC relays.

USAGE
    py host_kex.py COM6 COM7              # two boards
    py host_kex.py COM6 --sw-bob          # ONE board; Bob runs in software
    py host_kex.py COM6 --sw-bob --tamper # ...with a flipped ciphertext bit

ONE-BOARD MODE (--sw-bob)
    With a single KC705, the FPGA plays Alice and a software ML-KEM plays
    Bob. This is arguably the STRONGER demonstration: the two sides are
    independent implementations, so agreement cannot be explained by a
    shared bug. It prefers kyber-py if installed and otherwise falls back to
    the bundled reference model, which itself reproduces NIST's published
    ACVP vectors exactly (25/25 keyGen, 25/25 encaps, 10/10 decaps).

NOTE ON RANDOMNESS
    d, z (Alice's key seeds) and m (Bob's encapsulation randomness) are
    supplied by this script, because the boards have no TRNG -- see the
    randomness discussion in BOARD_BRINGUP.md. In a deployment each board
    would generate its own from a validated entropy source. Using fixed
    values here also makes the demo reproducible, which is useful on stage.
"""

import sys
import time
import os

try:
    import serial
except ImportError:
    sys.exit("pyserial not installed.  Run:  py -m pip install pyserial")

BAUD = 115200
C_PING, PING_R = 0xA5, 0x5A
C_WR, C_RD, C_SEED = 0x10, 0x11, 0x12
C_KG, C_EN, C_DE = 0x20, 0x21, 0x22
C_STAT, C_CYC, C_RST = 0x30, 0x31, 0x40

# Byte map -- must match mlkem_system.sv
CT_BASE, DK_BASE, MSG_BASE = 0, 1024, 1868
EK_BASE, HEK_BASE, Z_BASE, SS_BASE = 2048, 2900, 3100, 3900


class Board:
    def __init__(self, port, name):
        self.name = name
        self.ser = serial.Serial(port, BAUD, timeout=3.0)
        time.sleep(0.2)
        self.ser.reset_input_buffer()

    def _expect(self, want, what):
        got = self.ser.read(1)
        if not got:
            raise TimeoutError(f"[{self.name}] {what}: no reply")
        if got[0] != want:
            raise ValueError(f"[{self.name}] {what}: got 0x{got[0]:02X}")

    def ping(self, retries=3):
        for a in range(retries):
            try:
                self.ser.reset_input_buffer()
                self.ser.write(bytes([C_PING]))
                self._expect(PING_R, "PING")
                return
            except (TimeoutError, ValueError):
                if a == retries - 1:
                    raise
                time.sleep(0.3)

    def wr(self, addr, b):
        self.ser.write(bytes([C_WR, addr & 0xFF, (addr >> 8) & 0xFF, b]))
        self._expect(C_WR, f"WR[{addr}]")

    def rd(self, addr):
        self.ser.write(bytes([C_RD, addr & 0xFF, (addr >> 8) & 0xFF]))
        b = self.ser.read(1)
        if not b:
            raise TimeoutError(f"[{self.name}] RD[{addr}]")
        return b[0]

    def wr_block(self, addr, data):
        for i, b in enumerate(data):
            self.wr(addr + i, b)

    def rd_block(self, addr, n):
        return bytes(self.rd(addr + i) for i in range(n))

    def seed(self, data):
        for i, b in enumerate(data):
            self.ser.write(bytes([C_SEED, i, b]))
            self._expect(C_SEED, f"SEED[{i}]")

    def soft_reset(self):
        self.ser.write(bytes([C_RST]))
        self._expect(C_RST, "RESET")
        time.sleep(0.05)

    def status(self):
        self.ser.write(bytes([C_STAT]))
        b = self.ser.read(1)
        if not b:
            raise TimeoutError(f"[{self.name}] STATUS")
        return {"busy": bool(b[0] & 1), "done": bool(b[0] & 2),
                "reject": bool(b[0] & 4)}

    def cycles(self):
        v = 0
        for i in range(4):
            self.ser.write(bytes([C_CYC]))
            b = self.ser.read(1)
            if not b:
                raise TimeoutError(f"[{self.name}] CYCLES")
            v |= b[0] << (8 * i)
        return v

    def run(self, cmd, what, timeout_s=60.0):
        self.ser.write(bytes([cmd]))
        self._expect(cmd, f"START {what}")
        t0 = time.time()
        while time.time() - t0 < timeout_s:      # wait for busy to RISE
            if self.status()["busy"]:
                break
        while time.time() - t0 < timeout_s:      # ...then to FALL
            st = self.status()
            if not st["busy"]:
                return st
        raise TimeoutError(f"[{self.name}] {what} timed out")


class SoftwareBob:
    """Bob as an independent software implementation.

    Prefers kyber-py (third party). Falls back to the bundled reference
    model, which passes NIST's published ACVP vectors byte-for-byte.
    """

    def __init__(self):
        self.impl = None
        try:
            from kyber_py.ml_kem import ML_KEM_512
            self._encaps = lambda ek, m: ML_KEM_512._encaps_internal(ek, m)
            self.impl = "kyber-py (independent third-party implementation)"
        except ImportError:
            sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
            try:
                from golden_mlkem import mlkem_encaps
            except ImportError:
                sys.exit("Need either kyber-py (py -m pip install kyber-py) "
                         "or golden_mlkem.py beside this script.")
            self._encaps = lambda ek, m: mlkem_encaps(ek, m)
            self.impl = "bundled reference model (NIST ACVP-conformant)"

    def encaps(self, ek, m):
        return self._encaps(ek, m)


def main():
    tamper = "--tamper" in sys.argv
    sw_bob = "--sw-bob" in sys.argv
    ports = [a for a in sys.argv[1:] if not a.startswith("--")]
    if sw_bob and len(ports) < 1:
        sys.exit(f"usage: {sys.argv[0]} <alice-port> --sw-bob [--tamper]")
    if not sw_bob and len(ports) < 2:
        sys.exit(f"usage: {sys.argv[0]} <alice-port> <bob-port> [--tamper]\n"
                 f"   or: {sys.argv[0]} <alice-port> --sw-bob [--tamper]")

    print("=" * 66)
    print("  POST-QUANTUM KEY EXCHANGE ACROSS TWO FPGAs  (ML-KEM-512)")
    print("=" * 66)

    alice = Board(ports[0], "ALICE")
    alice.ping(); alice.soft_reset()
    if sw_bob:
        bob = SoftwareBob()
        print(f"ALICE = KC705 on {ports[0]}")
        print(f"BOB   = {bob.impl}\n")
    else:
        bob = Board(ports[1], "BOB")
        bob.ping(); bob.soft_reset()
        print(f"ALICE on {ports[0]}, BOB on {ports[1]} -- both alive\n")

    # Seeds. In deployment each board draws these from its own entropy source.
    d = os.urandom(32)
    z = os.urandom(32)
    m = os.urandom(32)

    # ---------- 1. Alice generates a keypair ----------
    alice.soft_reset()
    alice.seed(d)
    alice.wr_block(Z_BASE, z)
    alice.run(C_KG, "KeyGen")
    cyc = alice.cycles()
    ek = alice.rd_block(EK_BASE, 800)
    print(f"[ALICE] KeyGen  {cyc} cycles = {cyc/100.0:.0f} us")
    print(f"[ALICE] public key ek = {ek[:20].hex()}...  ({len(ek)} bytes)\n")

    # ---------- 2. ek travels to Bob ----------
    print(f"        --- ek sent over the link ({len(ek)} bytes) --->\n")

    # ---------- 3. Bob encapsulates ----------
    if sw_bob:
        t0 = time.time()
        k_bob, c = bob.encaps(ek, m)
        print(f"[BOB]   Encaps in software  {(time.time()-t0)*1e6:.0f} us")
    else:
        bob.wr_block(EK_BASE, ek)
        bob.wr_block(MSG_BASE, m)
        bob.run(C_EN, "Encaps")
        cyc = bob.cycles()
        c = bob.rd_block(CT_BASE, 768)
        k_bob = bob.rd_block(SS_BASE, 32)
        print(f"[BOB]   Encaps  {cyc} cycles = {cyc/100.0:.0f} us")
    print(f"[BOB]   ciphertext c = {c[:20].hex()}...  ({len(c)} bytes)")
    print(f"[BOB]   shared secret = {k_bob.hex()}\n")

    if tamper:
        bad = bytearray(c)
        bad[100] ^= 0x01
        c = bytes(bad)
        print("        !!! one ciphertext bit flipped in transit !!!\n")

    # ---------- 4. c travels back to Alice ----------
    print(f"        <--- c sent back over the link ({len(c)} bytes) ---\n")
    # Reset before Decaps. host_nist.py resets before EVERY operation and
    # passes 25/25 + 10/10; running KeyGen and Decaps back-to-back without one
    # is the one thing this flow did differently.
    alice.soft_reset()
    alice.wr_block(CT_BASE, c)
    alice.wr_block(Z_BASE + 32, c)      # J hashes z || c

    # ---------- 5. Alice decapsulates ----------
    st = alice.run(C_DE, "Decaps")
    cyc = alice.cycles()
    k_alice = alice.rd_block(SS_BASE, 32)
    print(f"[ALICE] Decaps  {cyc} cycles = {cyc/100.0:.0f} us   "
          f"reject={st['reject']}")
    print(f"[ALICE] shared secret = {k_alice.hex()}\n")

    # ---------- diagnostic ----------
    # If the board rejected, work out whether the CIPHERTEXT is genuinely
    # inconsistent with Alice's key, or whether the board simply mis-decapsulated
    # a perfectly valid one. The host can reconstruct Alice's full dk from what
    # the board already exposes, and run Decaps in software.
    if st["reject"] and not tamper:
        try:
            sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
            from golden_mlkem import mlkem_decaps
            dk_pke = alice.rd_block(DK_BASE, 768)
            hek = alice.rd_block(HEK_BASE, 32)
            dk_full = dk_pke + ek + hek + z
            k_sw = mlkem_decaps(dk_full, c)
            print("  [diag] software Decaps with Alice's own key:")
            print(f"         {k_sw.hex()}")
            if k_sw == k_bob:
                print("  [diag] the ciphertext IS valid for Alice's key --")
                print("         software recovers Bob's secret from it.")
                print("         So the board mis-decapsulated a good ciphertext:")
                print("         a staging or state issue on the board, not a")
                print("         key-agreement failure.")
            else:
                print("  [diag] software ALSO fails to recover Bob's secret --")
                print("         so ek or c did not survive the transfer intact.")
            # narrow it further
            print(f"  [diag] H(ek) on board  = {hek.hex()[:32]}...")
            import hashlib
            print(f"  [diag] H(ek) recomputed = "
                  f"{hashlib.sha3_256(ek).hexdigest()[:32]}...")
            zb = alice.rd_block(Z_BASE, 32)
            print(f"  [diag] z survived KeyGen: {zb == z}")
        except ImportError:
            print("  [diag] golden_mlkem.py not found; skipping diagnostic")
        print()

    # ---------- 6. Result ----------
    print("=" * 66)
    if tamper:
        if k_alice != k_bob and st["reject"]:
            print("  PASS -- tampered ciphertext REJECTED.")
            print("  Alice derived a different secret, exactly as the FO")
            print("  transform requires. The two boards do NOT agree, which")
            print("  is the correct outcome when the link is attacked.")
            rc = 0
        else:
            print("  FAIL -- tampering was not detected.")
            rc = 1
    else:
        if k_alice == k_bob:
            print("  PASS -- BOTH SIDES HOLD THE SAME 32-BYTE SECRET"
                  if sw_bob else
                  "  PASS -- BOTH FPGAs HOLD THE SAME 32-BYTE SECRET")
            print(f"          {k_alice.hex()}")
            print()
            print("  Only the public key and the ciphertext crossed the link.")
            print("  Neither secret was ever transmitted.")
            if sw_bob:
                print()
                print("  Alice's secret was computed entirely inside the FPGA;")
                print("  Bob's by a separate implementation. Two independent")
                print("  implementations, one in hardware, same 32 bytes.")
            else:
                print("  The PC computed no part of the KEM -- it only")
                print("  relayed bytes between the two FPGAs.")
            rc = 0
        else:
            print("  FAIL -- the two secrets differ")
            print(f"    ALICE {k_alice.hex()}")
            print(f"    BOB   {k_bob.hex()}")
            rc = 1
    print("=" * 66)
    return rc


if __name__ == "__main__":
    sys.exit(main())
