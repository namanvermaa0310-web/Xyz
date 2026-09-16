#!/usr/bin/env python3
"""
host_kex.py -- ML-KEM-512 KEY EXCHANGE  (definitive version)

    py host_kex.py COM8 COM6         two boards: Alice COM8, Bob COM6
    py host_kex.py COM6 --sw-bob     one board: Alice is the FPGA,
                                     Bob is a software implementation
    --tamper    flip a ciphertext bit and show the rejection path
    --verbose   print every staging step

PROTOCOL
    ALICE                                        BOB
    KeyGen -> ek (800 B), dk
              ek  ------ public key ------>
                                           Encaps -> K_bob (32 B), c (768 B)
              c   <----- ciphertext -------
    Decaps -> K_alice                      K_alice == K_bob

Only ek and c cross the link. Neither secret is ever transmitted; in two-board
mode the PC computes no part of the KEM.

BYTE MAP -- must match mlkem_system.sv exactly. A wrong offset here looks
identical to a cryptographic failure, which is why every large transfer below
is read back and verified.

    0    ..  767   ciphertext c
    1024 .. 1791   dk_pke
    1868 .. 1899   message m / m'
    1900 .. 1931   coins r
    2048 .. 2847   ek  (768 encoded + 32 rho)
    2900 .. 2931   H(ek)
    2932 .. 2995   G output  K || r
    3000 .. 3031   Kbar
    3100 .. 3931   z (32) then c (768)  -- J hashes z||c contiguously
    3900 .. 3931   shared secret K
    3950 .. 4717   saved copy of received c
    4750 .. 4813   staged m || H(ek) for G

THE BOARD DOES THESE ITSELF -- the host must not duplicate them:
    * Encaps computes H(ek) from ek in memory (do NOT stage H(ek) on Bob)
    * Encaps writes K to 3900 and c to 0
    * KeyGen writes ek including its rho tail, dk_pke and H(ek)
    * Decaps saves the received c before re-encrypting over it

THE HOST MUST STAGE
    Alice KeyGen : d (seed memory), z at 3100
    Bob   Encaps : ek at 2048, m at 1868
    Alice Decaps : c at 0, and c again at 3132 for J(z||c)
                   dk_pke / ek / H(ek) / z are already there from KeyGen
"""

import sys
import os
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial not installed.  Run:  py -m pip install pyserial")

BAUD = 115200
C_PING, PING_R = 0xA5, 0x5A
C_WR, C_RD, C_SEED = 0x10, 0x11, 0x12
C_KG, C_EN, C_DE = 0x20, 0x21, 0x22
C_STAT, C_CYC, C_RST = 0x30, 0x31, 0x40

CT_BASE, DK_BASE, MSG_BASE, COINS_BASE = 0, 1024, 1868, 1900
EK_BASE, HEK_BASE, G_BASE, KBAR_BASE = 2048, 2900, 2932, 3000
Z_BASE, SS_BASE, CSAVE_BASE, GIN_BASE = 3100, 3900, 3950, 4750

VERBOSE = False


def log(msg):
    if VERBOSE:
        print(f"        . {msg}")


class Board:
    def __init__(self, port, name):
        self.name, self.port = name, port
        self.ser = serial.Serial(port, BAUD, timeout=3.0)
        time.sleep(0.2)
        self.ser.reset_input_buffer()

    def _expect(self, want, what):
        got = self.ser.read(1)
        if not got:
            raise TimeoutError(f"[{self.name}] {what}: no reply")
        if got[0] != want:
            raise ValueError(
                f"[{self.name}] {what}: got 0x{got[0]:02X}, want 0x{want:02X}")

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

    def wr_verified(self, addr, data, what):
        """Write then read back. An 800-byte transfer that silently drops a
        byte produces a 'cryptographic' failure that is very hard to attribute,
        so it is turned into an explicit error here."""
        self.wr_block(addr, data)
        back = self.rd_block(addr, len(data))
        if back != data:
            n = next(i for i in range(len(data)) if back[i] != data[i])
            raise IOError(f"[{self.name}] {what}: readback differs at byte {n} "
                          f"(wrote {data[n]:02x}, read {back[n]:02x})")
        log(f"{self.name}: {what} verified, {len(data)} B at {addr}")

    def seed(self, data):
        for i, b in enumerate(data):
            self.ser.write(bytes([C_SEED, i, b]))
            self._expect(C_SEED, f"SEED[{i}]")
        log(f"{self.name}: seed d written")

    def soft_reset(self):
        self.ser.write(bytes([C_RST]))
        self._expect(C_RST, "RESET")
        time.sleep(0.05)
        log(f"{self.name}: soft reset")

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
        # Wait for busy to RISE then FALL. Polling `done` alone sees the
        # PREVIOUS operation's completion -- the level-held-done trap that hit
        # this design seven times in RTL applies to the host too.
        while time.time() - t0 < timeout_s:
            if self.status()["busy"]:
                break
        while time.time() - t0 < timeout_s:
            st = self.status()
            if not st["busy"]:
                return st
        raise TimeoutError(f"[{self.name}] {what} timed out")


class SoftwareBob:
    def __init__(self):
        try:
            from kyber_py.ml_kem import ML_KEM_512
            self._encaps = lambda ek, m: ML_KEM_512._encaps_internal(ek, m)
            self.impl = "kyber-py (independent third-party implementation)"
        except ImportError:
            sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
            try:
                from golden_mlkem import mlkem_encaps
            except ImportError:
                sys.exit("Need kyber-py or golden_mlkem.py beside this script.")
            self._encaps = lambda ek, m: mlkem_encaps(ek, m)
            self.impl = "bundled reference model (NIST ACVP-conformant)"

    def encaps(self, ek, m):
        return self._encaps(ek, m)


def diagnose(alice, ek, c, z, k_bob):
    """Separate 'the ciphertext is wrong' from 'the board mishandled a correct
    ciphertext'. Alice's full dk can be rebuilt from what the board exposes, so
    the identical decapsulation can be run in software."""
    print("  " + "-" * 62)
    print("  DIAGNOSTIC")
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from golden_mlkem import mlkem_decaps
    except ImportError:
        print("  golden_mlkem.py not found -- cannot diagnose further.")
        print("  " + "-" * 62)
        return
    import hashlib

    dk_pke = alice.rd_block(DK_BASE, 768)
    hek = alice.rd_block(HEK_BASE, 32)
    zb = alice.rd_block(Z_BASE, 32)
    ekb = alice.rd_block(EK_BASE, 800)

    print(f"  ek still intact on Alice : {ekb == ek}")
    print(f"  z survived KeyGen        : {zb == z}")
    print(f"  H(ek) on board correct   : "
          f"{hek == hashlib.sha3_256(ek).digest()}")

    try:
        k_sw = mlkem_decaps(dk_pke + ek + hek + z, c)
    except Exception as e:
        print(f"  software Decaps raised: {e}")
        print("  " + "-" * 62)
        return

    print(f"  software Decaps gives    : {k_sw.hex()[:32]}...")
    print(f"  Bob's secret was         : {k_bob.hex()[:32]}...")
    print()
    if k_sw == k_bob:
        print("  => The ciphertext IS valid for Alice's key -- software")
        print("     recovers Bob's secret from it. So the BOARD mishandled a")
        print("     correct ciphertext: a staging or state problem, not a key")
        print("     agreement failure.")
        print("     Next: run  host_nist.py <alice-port> decaps  to check the")
        print("     programmed bitstream still passes the NIST vectors.")
    else:
        print("  => Software ALSO fails, so ek or c did not survive the")
        print("     transfer, or Bob encapsulated against a different key.")
    print("  " + "-" * 62)


def main():
    global VERBOSE
    VERBOSE = "--verbose" in sys.argv
    tamper = "--tamper" in sys.argv
    sw_bob = "--sw-bob" in sys.argv
    ports = [a for a in sys.argv[1:] if not a.startswith("--")]

    if sw_bob and len(ports) < 1:
        sys.exit(f"usage: {sys.argv[0]} <alice-port> --sw-bob [--tamper]")
    if not sw_bob and len(ports) < 2:
        sys.exit(f"usage: {sys.argv[0]} <alice-port> <bob-port> [--tamper]\n"
                 f"   or: {sys.argv[0]} <alice-port> --sw-bob [--tamper]")

    print("=" * 68)
    print("  POST-QUANTUM KEY EXCHANGE  --  ML-KEM-512")
    print("=" * 68)

    alice = Board(ports[0], "ALICE")
    alice.ping()
    if sw_bob:
        bob = SoftwareBob()
        print(f"ALICE = KC705 on {ports[0]}")
        print(f"BOB   = {bob.impl}\n")
    else:
        bob = Board(ports[1], "BOB")
        bob.ping()
        print(f"ALICE = KC705 on {ports[0]}")
        print(f"BOB   = KC705 on {ports[1]}\n")

    d, z, m = os.urandom(32), os.urandom(32), os.urandom(32)

    # ---- 1. Alice: KeyGen ----
    # Reset before the operation: host_nist.py does this and passes the NIST
    # vectors, and running operations back-to-back has previously carried
    # stale state.
    alice.soft_reset()
    alice.seed(d)
    alice.wr_verified(Z_BASE, z, "z")
    alice.run(C_KG, "KeyGen")
    cyc = alice.cycles()
    ek = alice.rd_block(EK_BASE, 800)
    print(f"[ALICE] KeyGen  {cyc} cycles = {cyc/100.0:.0f} us")
    print(f"[ALICE] public key ek = {ek[:20].hex()}...  ({len(ek)} bytes)")

    if ek[768:] == bytes(32):
        print("\n  ERROR: ek[768:800] is all zeros -- rho was never written.")
        print("  The programmed bitstream predates the rho fix. Rebuild from")
        print("  the current mlkem_system.sv and reprogram both boards.")
        return 1
    print()

    # ---- 2. ek crosses ----
    print(f"        --- ek sent over the link ({len(ek)} bytes) --->\n")

    # ---- 3. Bob: Encaps ----
    if sw_bob:
        t0 = time.time()
        k_bob, c = bob.encaps(ek, m)
        print(f"[BOB]   Encaps in software  {(time.time()-t0)*1e6:.0f} us")
    else:
        bob.soft_reset()
        bob.wr_verified(EK_BASE, ek, "ek")      # Bob computes H(ek) itself
        bob.wr_verified(MSG_BASE, m, "m")
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

    # ---- 4. c crosses back ----
    print(f"        <--- c sent back over the link ({len(c)} bytes) ---\n")

    # ---- 5. Alice: Decaps ----
    # NO reset here: Alice must keep dk_pke, ek, H(ek) and z from her KeyGen.
    alice.wr_verified(CT_BASE, c, "c")
    alice.wr_verified(Z_BASE + 32, c, "c for J(z||c)")
    st = alice.run(C_DE, "Decaps")
    cyc = alice.cycles()
    k_alice = alice.rd_block(SS_BASE, 32)
    print(f"[ALICE] Decaps  {cyc} cycles = {cyc/100.0:.0f} us   "
          f"reject={st['reject']}")
    print(f"[ALICE] shared secret = {k_alice.hex()}\n")

    # ---- 6. Result ----
    print("=" * 68)
    if tamper:
        if k_alice != k_bob and st["reject"]:
            print("  PASS -- TAMPERED CIPHERTEXT REJECTED")
            print()
            print("  Alice derived a different secret, exactly as the")
            print("  Fujisaki-Okamoto transform requires. The two sides do NOT")
            print("  agree, which is correct when the link is attacked.")
            rc = 0
        else:
            print("  FAIL -- tampering was not detected")
            rc = 1
    elif k_alice == k_bob:
        print(f"  PASS -- {'BOTH SIDES' if sw_bob else 'BOTH FPGAs'} "
              f"HOLD THE SAME 32-BYTE SECRET")
        print(f"          {k_alice.hex()}")
        print()
        print("  Only the public key (800 B) and the ciphertext (768 B)")
        print("  crossed the link. Neither secret was ever transmitted.")
        if sw_bob:
            print("  Alice's secret came from the FPGA, Bob's from a separate")
            print("  implementation: two implementations, identical bytes.")
        else:
            print("  The PC computed no part of the KEM -- it relayed bytes.")
        rc = 0
    else:
        print("  FAIL -- the two secrets differ")
        print(f"    ALICE {k_alice.hex()}")
        print(f"    BOB   {k_bob.hex()}")
        print("=" * 68)
        diagnose(alice, ek, c, z, k_bob)
        return 1
    print("=" * 68)
    return rc


if __name__ == "__main__":
    sys.exit(main())
