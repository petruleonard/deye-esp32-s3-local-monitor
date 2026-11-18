#!/usr/bin/env python3
"""
Simulator Solarman V5 + UDP Discovery pentru invertor Deye Monofazat.
SN: 1234567890 | MAC: AABBCCDDEEFF

Asculta:
  - UDP 48899: raspunde la "WIFIKIT-214028-READ" cu "<IP>,AABBCCDDEEFF,1234567890"
  - TCP  8899: raspunde la cereri Solarman V5 (Modbus RTU func 0x03)
"""

import math
import os
import socket
import struct
import threading
import time

LOGGER_SN = 1234567890
LOGGER_MAC = "AABBCCDDEEFF"
UDP_DISC_PORT = 48899
TCP_V5_PORT = 8899
OUTAGE_FLAG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "grid_outage.flag")
FAST_24H_FLAG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fast_24h.flag")


def get_lan_ip(target_ip: str = "8.8.8.8") -> str:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((target_ip, 80))
        return s.getsockname()[0]
    except Exception:
        return "127.0.0.1"
    finally:
        s.close()


def modbus_crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc


def build_deye_registers(t0: float) -> dict[int, int]:
    """Genereaza valori realiste si dinamice pentru un invertor Deye monofazat."""
    elapsed = time.time() - t0
    wave = math.sin(elapsed * 0.25)
    wave2 = math.cos(elapsed * 0.18)

    if os.path.exists(FAST_24H_FLAG):
        # Simulare 24h in 120 secunde (0..24h)
        hour = (elapsed % 120.0) / 120.0 * 24.0
        # Productie PV: clopot intre 06:00 si 19:00, varf ~4500W la 12:30
        if 6.0 <= hour <= 19.0:
            sun = math.sin(((hour - 6.0) / 13.0) * math.pi)
            pv_total_w = int(max(0.0, 4500.0 * (sun ** 1.35) + 120.0 * wave))
        else:
            pv_total_w = 0
        pv1_w = int(pv_total_w * 0.56)
        pv2_w = pv_total_w - pv1_w

        # Consum casa: baza 450W noaptea, varf dimineata (08h) si seara (18-22h)
        morning_peak = 900.0 * math.exp(-((hour - 8.0) ** 2) / 2.2)
        evening_peak = 2200.0 * math.exp(-((hour - 20.0) ** 2) / 4.5)
        day_base = 700.0 if 7.0 <= hour <= 22.0 else 420.0
        load_w = int(day_base + morning_peak + evening_peak + 80.0 * wave2)

        # Bateria: se incarca intre 08-14h, se descarca seara, SOC 25%..98%
        if hour < 6.0:
            soc = int(38 - (hour / 6.0) * 13)
        elif hour < 14.5:
            soc = int(25 + ((hour - 6.0) / 8.5) * 73)
        elif hour < 17.5:
            soc = 98
        else:
            soc = int(98 - ((hour - 17.5) / 6.5) * 60)
        soc = max(15, min(100, soc))

        surplus = pv_total_w - load_w
        if surplus > 300 and soc < 98:
            bat_w = int(min(2200, surplus * 0.65))  # incarcare
        elif surplus < -200 and soc > 20 and 17.5 <= hour <= 23.0:
            bat_w = -int(min(1800, abs(surplus) * 0.55))  # descarcare partiala seara (lasa si import!)
        else:
            bat_w = 0

        # Grid = Load + Bat - PV (<0 export, >0 import)
        grid_w = load_w + bat_w - pv_total_w
    else:
        pv1_w = int(2150 + 250 * wave)
        pv2_w = int(1720 + 200 * wave2)
        pv_total_w = pv1_w + pv2_w

        load_w = int(1250 + 180 * math.sin(elapsed * 0.35 + 1.0))
        bat_w = int(1400 + 150 * wave)  # >0 = incarcare
        # Bilant aproximativ: Grid = Load + Bat - PV (<0 export, >0 import)
        grid_w = load_w + bat_w - pv_total_w

        soc = max(10, min(100, int(82 + 3 * math.sin(elapsed * 0.05))))
    bat_v = 53.40 + 0.3 * wave
    bat_a = bat_w / bat_v

    regs: dict[int, int] = {}

    # Reg 0: Device Type (0x0300 = Hybrid 1-Phase)
    regs[0] = 0x0300

    # --- Block 2: 86..113 (Daily totals & PV voltages) ---
    regs[86] = 1000 + int((38.4 + 1.2 * wave) * 10)  # DC Temp (offset 1000, 0.1 C)
    regs[90] = 2                                      # Inverter status: 2 = Normal
    regs[96] = 48                                     # Battery charge today: 4.8 kWh
    regs[97] = 19                                     # Battery discharge today: 1.9 kWh
    regs[104] = 12                                    # Grid buy today: 1.2 kWh
    regs[105] = 94                                    # Grid sell today: 9.4 kWh
    regs[108] = 186                                   # PV today: 18.6 kWh
    regs[109] = int((362.5 + 3.0 * wave) * 10)        # PV1 Voltage (0.1V)
    regs[110] = 104                                   # Load today: 10.4 kWh
    regs[111] = int((358.2 + 2.5 * wave2) * 10)       # PV2 Voltage (0.1V)

    # --- Block 1: 150..197 (Realtime power & battery) ---
    is_outage = os.path.exists(OUTAGE_FLAG)
    if is_outage:
        grid_w = 0
        bat_w = load_w - pv_total_w  # bateria/solarul sustin casa
        regs[150] = 0                                 # Grid L1 Voltage: 0.0 V (PANA RETEA!)
    else:
        regs[150] = int((232.4 + 1.5 * wave) * 10)    # Grid L1 Voltage (0.1V)
    regs[169] = grid_w & 0xFFFF                       # Grid Power (signed 16-bit)
    regs[178] = load_w & 0xFFFF                       # Load Power (W)
    regs[182] = 1000 + 265                            # Battery Temp: 26.5 C
    regs[184] = soc                                   # Battery SOC (%)
    regs[185] = 99                                    # Battery SOH (%)
    regs[186] = pv1_w & 0xFFFF                        # PV1 Power (W)
    regs[187] = pv2_w & 0xFFFF                        # PV2 Power (W)
    regs[190] = bat_w & 0xFFFF                        # Battery Power (signed 16-bit)
    regs[191] = int(bat_a * 100) & 0xFFFF             # Battery Current (0.01A signed)
    regs[195] = 5000                                  # Grid Frequency: 50.00 Hz
    regs[196] = int(bat_v * 100) & 0xFFFF             # Battery Voltage (0.01V)

    return regs


def udp_discovery_loop() -> None:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", UDP_DISC_PORT))
    print(f"[UDP {UDP_DISC_PORT}] Ascult pachete de descoperire Solarman...")

    while True:
        try:
            data, addr = sock.recvfrom(1024)
            msg = data.decode("ascii", errors="ignore").strip()
            if "WIFIKIT-214028-READ" in msg:
                my_ip = get_lan_ip(addr[0])
                reply = f"{my_ip},{LOGGER_MAC},{LOGGER_SN}".encode("ascii")
                sock.sendto(reply, addr)
                print(f"[UDP {UDP_DISC_PORT}] Discovery de la {addr[0]}:{addr[1]} -> raspuns: {reply.decode()}")
        except Exception as e:
            print(f"[UDP] Eroare: {e}")


def handle_tcp_client(conn: socket.socket, addr: tuple[str, int], t0: float) -> None:
    try:
        conn.settimeout(3.0)
        req = conn.recv(512)
        if len(req) < 30 or req[0] != 0xA5 or req[-1] != 0x15:
            return

        # Request V5: 11 header + 15 metadata = 26 bytes -> Modbus RTU incepe la byte 26
        seq = req[5:7]
        mb_req = req[26:-2]
        if len(mb_req) < 8:
            return

        slave_id = mb_req[0]
        func_code = mb_req[1]
        start_reg = (mb_req[2] << 8) | mb_req[3]
        num_regs = (mb_req[4] << 8) | mb_req[5]

        if func_code != 0x03:
            return

        reg_map = build_deye_registers(t0)
        payload_bytes = bytearray()
        for r in range(start_reg, start_reg + num_regs):
            val = reg_map.get(r, 0)
            payload_bytes.extend(struct.pack(">H", val))

        mb_body = bytes([slave_id, 0x03, len(payload_bytes)]) + bytes(payload_bytes)
        crc = modbus_crc16(mb_body)
        mb_resp = mb_body + struct.pack("<H", crc)

        # Response V5: 11 header + 14 metadata = 25 bytes -> mb_resp incepe la byte 25
        v5_payload_len = 14 + len(mb_resp)
        resp = bytearray()
        resp.append(0xA5)
        resp.extend(struct.pack("<H", v5_payload_len))
        resp.append(0x10)
        resp.append(0x15)  # Response control code
        resp.extend(seq)
        resp.extend(struct.pack("<I", LOGGER_SN))

        # 14 bytes metadata (frame_type=0x02, status=0x01, 12 bytes timp/offset)
        resp.append(0x02)
        resp.append(0x01)
        resp.extend(b"\x00" * 12)

        resp.extend(mb_resp)
        cs = sum(resp[1:]) & 0xFF
        resp.append(cs)
        resp.append(0x15)

        conn.sendall(bytes(resp))
        print(
            f"[TCP {TCP_V5_PORT}] {addr[0]} a citit reg {start_reg}..{start_reg + num_regs - 1} "
            f"(PV1={reg_map[186]}W, PV2={reg_map[187]}W, Bat={reg_map[184]}%, Load={reg_map[178]}W)"
        )
    except Exception as e:
        print(f"[TCP] Eroare client {addr[0]}: {e}")
    finally:
        conn.close()


def tcp_server_loop(t0: float) -> None:
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", TCP_V5_PORT))
    srv.listen(8)
    print(f"[TCP {TCP_V5_PORT}] Simulator Solarman V5 pornit (SN={LOGGER_SN}, MAC={LOGGER_MAC})")

    while True:
        conn, addr = srv.accept()
        threading.Thread(target=handle_tcp_client, args=(conn, addr, t0), daemon=True).start()


if __name__ == "__main__":
    t_start = time.time()
    lan_ip = get_lan_ip()
    print("============================================================")
    print("  Simulator Logger Solarman V5 - Deye Monofazat")
    print(f"  IP PC LAN : {lan_ip}")
    print(f"  Logger SN : {LOGGER_SN}")
    print(f"  Logger MAC: {LOGGER_MAC}")
    print("============================================================")

    t_udp = threading.Thread(target=udp_discovery_loop, daemon=True)
    t_udp.start()

    try:
        tcp_server_loop(t_start)
    except KeyboardInterrupt:
        print("\nSimulator oprit.")
