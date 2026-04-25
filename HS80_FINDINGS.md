# Corsair HS80 RGB Wireless (0x0A6B) - Linux HID Protocol

## Strategia di Stabilità (Verificata)
Per evitare disconnessioni del dongle, la comunicazione deve seguire un ritmo costante e non eccessivo:
1. **Handshake**: Inviato ogni 10 secondi per mantenere la Software Mode.
2. **Polling Stato**: Ogni 100ms (ascolto passivo o timeout su Interfaccia 3).
3. **Sincronizzazione LED**: Inviare subito quando cambia lo stato microfono, con un intervallo minimo anti-spam, e mantenere comunque un keepalive ogni 2 secondi. Evitare una logica basata solo sugli eventi mic: se un evento viene perso o la software mode decade, il colore resta desincronizzato.

## 1. Handshake (Software Mode)
Inviare questi pacchetti all'Interfaccia 3:
- Enable Software Mode: `02 09 01 03 00 02`
- Open Lighting Endpoint: `02 09 0D 00 01`

## 2. LED Control (Interleaved RGB)
**Report ID**: 02, **Wireless Mode**: 09, **Command**: 06
Format: `02 09 06 00 09 00 00 00 [R0 R1 R2] [G0 G1 G2] [B0 B1 B2]`

### Mappatura Zone Verificata
- **Index 0**: Logo LED
- **Index 1**: Power LED
- **Index 2**: Microphone LED

### Esempio Stabile (Power Verde, Mic Dinamico)
- Power: `R1=0, G1=255, B1=0`
- Logo: `R0=0, G0=0, B0=0` (OFF)
- Mic: `R2=255/0, G2=0, B2=0` (Rosso se mutato, OFF se attivo)

## 3. Status Monitoring (Mute/Battery)
**Report ID**: 03 (Interfaccia 3)
**Event Code**: A6 (Microphone Status)
- **Byte 6 (Index 5)**: `01` (Muted/Up), `00` (Active/Down)

**Event Code**: 0F (Battery Level)
- **Byte 6-7 (Index 5-6)**: 16-bit Little Endian (Value / 10 = %)
