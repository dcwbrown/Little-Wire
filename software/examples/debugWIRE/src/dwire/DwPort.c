/// DebugPort.c

// DebugWire output command flag bits:
//
//     00000001    1     Send break
//     00000010    2     Set timing parameter
//     00000100    4     Send bytes
//     00001000    8     Wait for start bit
//     00010000   16     Read bytes
//     00100000   32     Read pulse widths
//
// Supported combinations
//    33 - Send break and read pulse widths
//     2 - Set timing parameters
//     4 - Send bytes
//    20 - Send bytes and read response (normal command)
//    28 - Send bytes, wait and read response (e.g. after programming, run to BP)
//    36 - Send bytes and receive 0x55 pulse widths
//
// Note that the wait for start bit loop also monitors the dwState wait for start
// bit flag, and is arranged so that sending a 33 (send break and read pulse widths)
// will abort a pending wait.


#include "littleWire.h"

#define VENDOR_ID   0x1781
#define PRODUCT_ID  0x0c9f
#define USB_TIMEOUT 5000
#define OUT_TO_LW   USB_TYPE_VENDOR | USB_RECIP_DEVICE | USB_ENDPOINT_OUT
#define IN_FROM_LW  USB_TYPE_VENDOR | USB_RECIP_DEVICE | USB_ENDPOINT_IN


void PortFail(char *msg) {usb_close(Port); Port = 0; Fail(msg);}


static uint32_t cyclesPerPulse;

int SetDwireBaud() { // returns 1 iff success
  int status;
  int tries;
  int i;
  uint16_t times[64];

  tries = 0;  status = 0;
  while ((tries < 5)  &&  (status <= 0)) {
    tries++;
    delay(20);
    // Read back timings
    status = usb_control_msg(Port, IN_FROM_LW, 60, 0, 0, (char*)times, sizeof(times), USB_TIMEOUT);
    //Ws("Read back timimgs status: "); Wd(status,1); Wsl(".");
  }
  if (status < 18) {return 0;}
  uint32_t measurementCount = status / 2;

  //Ws("  Pulse times:");
  //for (i=0; i<measurementCount; i++) {Wc(' '); Wd(times[i],1);}
  //Wsl(".");

  // Average measurements and determine baud rate as pulse time in device cycles.

  cyclesPerPulse = 0;
  for (i=measurementCount-9; i<measurementCount; i++) cyclesPerPulse += times[i];

  // Pulse cycle time for each measurement is 6*measurement + 8 cyclesPerPulse.
  cyclesPerPulse = (6*cyclesPerPulse) / 9  +  8;

  // Determine timing loop iteration counts for sending and receiving bytes

  times[0] = (cyclesPerPulse-8)/4;  // dwBitTime

  // Send timing parameters to digispark
  status = usb_control_msg(Port, OUT_TO_LW, 60, 2, 0, (char*)times, 2, USB_TIMEOUT);
  if (status < 0) {PortFail("Failed to set debugWIRE port baud rate");}

  return 1;
}




void DwBreakAndSync() {
  for (int tries=0; tries<25; tries++) {
    // Tell digispark to send a break and capture any returned pulse timings
    //Wsl("Commanding digispark break and capture.");
    int status = usb_control_msg(Port, OUT_TO_LW, 60, 33, 0, 0, 0, USB_TIMEOUT);
    if (status >= 0) {
      delay(120); // Wait while digispark sends break and reads back pulse timings
      // Get any pulse timings back from digispark
      if (SetDwireBaud()) {
        Ws("Connected at "); Wd(16500000 / cyclesPerPulse, 1); Wsl(" baud.");
        return;
      }
    }
    Wc('.'); Flush();
  }
  Wl(); PortFail("Digispark/LittleWire could not capture pulse timings after 25 break attempts.");
}




void ConnectPort() {
  usb_init();
  usbOpenDevice(&Port, VENDOR_ID, "*", PRODUCT_ID, "*", "*", NULL, NULL );
  if (!Port) {Fail("Couldn't connect to digispark.");}
  DwBreakAndSync();
}



int dwReachedBreakpoint() {
  char dwBuf[10];
  int status = usb_control_msg(Port, IN_FROM_LW, 60, 0, 0, dwBuf, sizeof(dwBuf), USB_TIMEOUT);
  //if (status < 0) {
  //  Ws("dwReachedBreakpoint: dwBuf read returned "); Wd(status,1);
  //  Ws(", dwBuf[0] = $"); Wx(dwBuf[0],2); Wsl(".");
  //}
  return status >= 0  &&  dwBuf[0] != 0;
}


// Low level send to device.

// state = 0x04 - Just send the bytes
// state = 0x14 - Send bytes and read response bytes
// state = 0x24 - Send bytes and record response pulse widths

void dwUSBSendBytes(u8 state, char *out, int outlen) {

  if (!Port) ConnectPort();

  int tries  = 0;
  int status = usb_control_msg(Port, OUT_TO_LW, 60, state, 0, out, outlen, USB_TIMEOUT);

  while ((tries < 50) && (status <= 0)) {
    // Wait for previous operation to complete
    tries++;
    delay(20);
    status = usb_control_msg(Port, OUT_TO_LW, 60, state, 0, out, outlen, USB_TIMEOUT);
  }
  if (status < outlen) {Ws("Failed to send bytes to AVR, status "); Wd(status,1); PortFail("");}
  delay(3); // Wait at least until digispark starts to send the data.
}


// Buffer accumulating debugWIRE data to be sent to the device.
// We buffer data in order to minimise the number of USB transactions used,
// but we also guarantee that a debugWIRE read transaction includes at leasr
// one byte of data to be sent first.

char OutBufBytes[128];
int  OutBufLength = 0;

void dwBufferFlush(u8 state) {
  if (OutBufLength > 0) {
    dwUSBSendBytes(state, OutBufBytes, OutBufLength);
    OutBufLength = 0;
  }
}


// Exposed APIs:
//
//   DwSend  - send bytes; (Bytes are buffered and will be sent on next
//                          DwFlush, DwReceive or DwCapture call.)
//   DwFlush - Flush buffer to device

void DwSend(const u8 *out, int outlen) {
  while (OutBufLength + outlen > sizeof(OutBufBytes)) {
    // Total (buffered and passed here) execeeds maximum transfer length (128
    // bytes = OutBuf size). Send buffered and new data until there remains
    // between 1 and 128 bytes still to send in the buffer.
    int lenToCopy = sizeof(OutBufBytes)-OutBufLength;
    memcpy(OutBufBytes+OutBufLength, out, lenToCopy);
    dwUSBSendBytes(0x04, OutBufBytes, sizeof(OutBufBytes));
    OutBufLength = 0;
    out += lenToCopy;
    outlen -= lenToCopy;
  }
  Assert(OutBufLength + outlen <= sizeof(OutBufBytes));
  memcpy(OutBufBytes+OutBufLength, out, outlen);
  OutBufLength += outlen;
  // Remainder stays in buffer to be sent with next read request, or on a
  // flush call.
}


void DwFlush() {
  dwBufferFlush(0x14);
}


int DwReceive(u8 *in, int inlen) {
  Assert(inlen <= 128);
  int tries  = 0;
  int status = 0;

  dwBufferFlush(0x14);

  while ((tries < 50) && (status <= 0)) {
    tries++;
    delay(20);
    // Read back dWIRE bytes
    status = usb_control_msg(Port, IN_FROM_LW, 60, 0, 0, (char*)in, inlen, USB_TIMEOUT);
  }
  return status;
}

int DwReadByte() {u8 byte = 0; DwReceive(&byte, 1); return byte;}
int DwReadWord() {u8 buf[2] = {0}; DwReceive(buf, 2); return (buf[0] << 8) | buf[1];}


void DwSync() {
  dwBufferFlush(0x24);
  if (!SetDwireBaud()) {PortFail("Could not read back timings following transfer and sync command");}
}

void DwWait() {
  dwBufferFlush(0x0C);  // Send bytes and wait for dWIRE line state change
}








u8 hi(int w) {return (w>>8)&0xff;}
u8 lo(int w) {return (w   )&0xff;}


void SetSizes(int signature) {
  int i=0; while (Characteristics[i].signature  &&  Characteristics[i].signature != signature) {i++;}

  if (Characteristics[i].signature) {
    DeviceType = i;
    Ws("Device recognised as "); Wsl(Characteristics[DeviceType].name);
  } else {
    DeviceType = -1;
    Ws("Unrecognised device signature: "); Wx(signature,4); Fail("");
  }
}




void DwSetPC(u16 pc) {DwSend(Bytes(0xD0, hi(pc)|0x10, lo(pc)));}
void DwSetBP(u16 bp) {DwSend(Bytes(0xD1, hi(bp)|0x10, lo(bp)));}

void DwInst(u16 inst) {DwSend(Bytes(0xD2, hi(inst), lo(inst), 0x23));}

void DwIn(u8 reg, u16 ioreg)  {DwInst(0xB000 | ((ioreg << 5) & 0x600) | ((reg << 4) & 0x01F0) | (ioreg & 0x000F));}
void DwOut(u16 ioreg, u8 reg) {DwInst(0xB800 | ((ioreg << 5) & 0x600) | ((reg << 4) & 0x01F0) | (ioreg & 0x000F));}



// Register access
//


void DwGetRegs(int first, u8 *regs, int count) {
  if (count == 1) {
    DwOut(DWDRreg(), first);
  } else {
    DwSetPC(first);
    DwSetBP(first+count);
    DwSend(Bytes(0x66, 0xC2, 1, 0x20)); // Start register read
  }
  DwReceive(regs, count);
}

void DwSetReg(int reg, u8 val) {DwIn(reg, DWDRreg()); DwSend(Bytes(val));}

void DwSetRegs(int first, const u8 *regs, int count) {
  if (count <= 3) {
    while (count > 0) {DwSetReg(first, *regs); first++; regs++; count--;}
  } else {
    DwSetPC(first);
    DwSetBP(first+count);
    DwSend(Bytes(0x66, 0xC2, 0x05, 0x20)); // Start register write
    DwSend(regs, count);
  }
}

void DwSetZ(u16 z) {DwSetRegs(30, (u8*)&z, 2);}



// Data area access
//


void DwUnsafeReadAddr(int addr, int len, u8 *buf) {
  // Do not read addresses 30, 31 or DWDR as these interfere with the read process
  DwSetZ(addr);
  DwSetPC(0);
  DwSetBP(2*len);
  DwSend(Bytes(0x66, 0xC2, 0x00, 0x20)); // Start data area read
  DwReceive(buf, len);
}

void DwReadAddr(int addr, int len, u8 *buf) {
  // Read range before r28
  int len1 = min(len, 28-addr);
  if (len1 > 0) {DwUnsafeReadAddr(addr, len1, buf); addr+=len1; len-=len1; buf+=len1;}

  // Some registers are cached - use the cached values.
  while (addr >= 28  &&  addr <= 31  &&  len > 0) {
    *buf = R[addr];  addr++;  len--;  buf++;
  }

  // Read range from 32 to DWDR
  int len2 = min(len, DWDRaddr()-addr);
  if (len2 > 0) {DwUnsafeReadAddr(addr, len2, buf); addr+=len2; len-=len2; buf+=len2;}

  // Provide dummy 0 value for DWDR
  if (addr == DWDRaddr()  &&  len > 0) {buf[0] = 0; addr++; len--; buf++;}

  // Read anything beyond DWDR no more than 128 bytes at a time
  while (len > 128) {DwUnsafeReadAddr(addr, 128, buf); addr+=128; len -=128; buf+=128;}
  if (len > 0) {DwUnsafeReadAddr(addr, len, buf);}
}


void DwWriteAddr(int addr, int len, const u8 *buf) {
  DwSetZ(addr);
  DwSetBP(3);
  DwSend(Bytes(0x66, 0xC2, 0x04)); // Set data area write mode
  int limit = addr + len;
  while (addr < limit) {
    if (addr < 28  ||  (addr > 31  &&  addr != DWDRaddr())) {
      DwSetPC(1);
      DwSend(Bytes(0x20, *buf)); // Write one byte to data area and increment Z
    } else {
      if (addr >= 28  &&  addr <= 31) {R[addr] = *buf;}
      DwSetZ(addr+1);
    }
    addr++;
    buf++;
  }
}



// Start / stop
//


void DwReconnect() {
  DwSend(Bytes(0xF0));  // Request current PC
  PC = (2 * (DwReadWord() - 1) % FlashSize());
  DwGetRegs(28, R+28, 4); // Cache r28 through r31
}

void DwConnect() {
  DwSend(Bytes(0xF3));  // Request signature
  SetSizes(DwReadWord());
  DwReconnect();
}

void DwReset() {
  DwSend(Bytes(0x07));  // dWIRE reset
  DwSync();
  DwReconnect();
}

void DwDisable() {
  DwSend(Bytes(0x06));
  DwFlush();
}


void DwTrace() { // Execute one instruction
  DwSetRegs(28, R, 4);       // Restore cached registers
  DwSetPC(PC/2);             // Trace start address
  DwSend(Bytes(0x60, 0x31)); // Single step
  DwSync();
  DwReconnect();
}


void DwGo() { // Begin executing.
  DwSetRegs(28, R, 4);  // Restore cached registers
  DwSetPC(PC/2);        // Execution start address
  if (BP < 0) {         // Prepare to start execution with no breakpoint set
    DwSend(Bytes(TimerEnable ? 0x40 : 0x60)); // Set execution context
  } else {              // Prpare to start execution with breakpoint set
    DwSetBP(BP/2);      // Breakpoint address
    DwSend(Bytes(TimerEnable ? 0x41 : 0x61)); // Set execution context including breakpoint enable
  }
  DwSend(Bytes(0x30)); // Continue execution (go)
  DwWait();
}







/*

/// DebugWire protocol notes

See RikusW's excellent work at http://www.ruemohr.org/docs/debugwire.html.


DebugWire command byte interpretation:

06      00 xx x1 10   Disable dW (Enable ISP programming)
07      00 xx x1 11   Reset

20      00 10 00 00   go start reading/writing SRAM/Flash based on low byte of IR
21      00 10 00 01   go read/write a single register
23      00 10 00 11   execute IR (single word instruction loaded with D2)

30      00 11 00 00   go normal execution
31      00 11 00 01   single step (Rikusw says PC increments twice?)
32      00 11 00 10   go using loaded instruction
33      00 11 00 11   single step using slow loaded instruction (specifically spm)
                      will generate break and 0x55 output when complete.

t: disable timers
40/60   01 t0 00 00   Set GO context  (No bp?)
41/61   01 t0 00 01   Set run to cursor context (Run to hardware BP?)
43/63   01 t0 00 11   Set step out context (Run to return instruction?)
44/64   01 t0 01 00   Set up for single step using loaded instruction
46/66   01 t0 01 10   Set up for read/write using repeating simulated instructions
59/79   01 t1 10 01   Set step-in / autostep context or when resuming a sw bp (Execute a single instruction?)
5A/7A   01 t1 10 10   Set single step context



83      10 d0 xx dd   Clock div

C2      11 00 00 10   Set read/write mode (folowed by 0/4 SRAM, 1/5 regs, 2 flash)

w:  word operation (low byte only if 0)
cc: control regs: 0: PC, 1: BP, 2: IR, 3: Sig.
Cx/Dx   11 0w xx cc   Set control reg  (Cx for byte register, Dx for word register)
Ex/Fx   11 1w xx cc   Read control reg (Ex for byte register, Fx for word register)


Modes:

SRAM repeating instructions:
C2 00                   C2 04
ld  r16,Z+       or     in r16,DWDR
out DWDR,r16     or     st Z+,r16

Regs repeating instructions
C2 01            or     C2 05
out DWDR,r0      or     in r0,DWDR
out DWDR,r1      or     in r1,DWDR
out DWDR,r2      or     in r2,DWDR
...                     ....

Flash repeating instructions
C2 03
lpm r?,Z+        or    ?unused
out SWDR,r?      or    ?unused



-------------------------------------------------------------------------



40/60   0 1  x  0 0  0  0 0   GO                         Set GO context  (No bp?)
41/61   0 1  x  0 0  0  0 1   Run to cursor              Set run to cursor context (Run to hardware BP?)
43/63   0 1  x  0 0  0  1 1   Step out                   Set step out context (Run to return instruction?)
44/64   0 1  x  0 0  1  0 0   Write flash page           Set up for single step using loaded instruction
46/66   0 1  x  0 0  1  1 0   Use virtual instructions   Set up for read/write using repeating simulated instructions
59/79   0 1  x  1 1  0  0 1   Step in/autostep           Set step-in / autostep context or when resuming a sw bp (Execute a single instruction?)
5A/7A   0 1  x  1 1  0  1 0   Single step                Set single step context
             |  | |  |  | |
             |  | |  |  '-'------ 00 no break
             |  | |  |  '-'------ 01 break when PC = BP, or single step resuming a sw bp
             |  | |  |  '-'------ 10 Used for executing from virtual space OR single step
             |  | |  |  '-'------ 11 break at return?
             |  | |  '----------- Instructions will load from flash (0) or virtual space (1)
             |  '-'-------------- 00 Not single step
             |  '-'-------------- 01 ?
             |  '-'-------------- 10 ?
             |  '-'-------------- 11 Single step or maybe, use IR instead of (PC) for first instruction
             '------------------- Run with timers disabled


20      0 0 1 0 0 0 0 0    go start reading/writing reg/SRAM/Flash based on IR and low byte of PC
21      0 0 1 0 0 0 0 1    single step read/write a single register
22      0 0 1 0 0 0 1 0    MAYBE go starting with instruction in IR followed by virtual instrucion?
23      0 0 1 0 0 0 1 1    single step an instruction in IR (loaded with D2)
30      0 0 1 1 0 0 0 0    go normal execution
31      0 0 1 1 0 0 0 1    single step (Rikusw says PC increments twice?)
32      0 0 1 1 0 0 1 0    go using loaded instruction
33      0 0 1 1 0 0 1 1    single step using slow loaded instruction (specifically spm)
              |     | |    will generate break and 0x55 output when complete.
              |     | |
              |     | '--- Single step - stop after 1 instruction
              |-----'----- 00 Execute from virtual space
              |-----'----- 01 Execute from loaded IR
              |-----'----- 10 Execute from flash
              |-----'----- 11 Execute from loaded IR and generate break on completion (specifically fro SPM)


Resume execution:              60/61/79/7A 30
Resume from SW BP:             79 32
Step out:                      63 30
Execute instruction (via D2):  ?? 23
Read/write registers/SRAM:     66 20
Write single register:         66 21



Resuming execution

D0 00 00 xx -- set PC, xx = 40/60 - 41/61 - 59/79 - 5A/7A
D1 00 01 -- set breakpoint (single step in this case)
D0 00 00 30 -- set PC and GO







Writing a Flash Page

66
D0 00 1A D1 00 20 C2 05 20 03 01 05 40 00 00 -- Set X, Y, Z
D0 1F 00                                     -- Set PC to 0x1F00, inside the boot section to enable spm--

64
D2  01 CF  23        -- movw r24,r30
D2  BF A7  23        -- out SPMCSR,r26 = 03 = PGERS
D2  95 E8  33        -- spm

<00 55> 83 <55>

44 - before the first one
And then repeat the following until the page is full.

D0  1F 00            -- set PC to bootsection for spm to work
D2  B6 01  23 ll     -- in r0,DWDR (ll)
D2  B6 11  23 hh     -- in r1,DWDR (hh)
D2  BF B7  23        -- out SPMCSR,r27 = 01 = SPMEN
D2  95 E8  23        -- spm
D2  96 32  23        -- adiw Z,2


D0 1F 00
D2 01 FC 23 movw r30,r24
D2 BF C7 23 out SPMCSR,r28 = 05 = PGWRT
D2 95 E8 33 spm
<00 55>

D0 1F 00
D2 E1 C1 23 ldi r28,0x11
D2 BF C7 23 out SPMCSR,r28 = 11 = RWWSRE
D2 95 E8 33 spm
<00 55> 83 <55>

Reading Eeprom

66 D0 00 1C D1 00 20 C2 05 20 --01 01 00 00-- --Set YZ--
64 D2 BD F2 23 D2 BD E1 23 D2 BB CF 23 D2 B4 00 23 D2 BE 01 23 xx

66 D0 00 1C D1 00 20 C2 05 20 --01 01 00 00-- --Set YZ--
64
D2 BD F2 23 out EEARH,r31
D2 BD E1 23 out EEARL,r30
D2 BB CF 23 out EECR,r28 = 01 = EERE
D2 B4 00 23 in r0,EEDR
D2 BE 01 23 out DWDR,r0
xx -- Byte from target


Writing Eeprom

66 D0 00 1A D1 00 20 C2 05 20 --04 02 01 01 10 00-- --Set XYZ--
64 D2 BD F2 23 D2 BD E1 23 D2 B6 01 23 xx D2 BC 00 23 D2 BB AF 23 D2 BB BF 23

64
D2 BD F2 23 out EEARH,r31 = 00
D2 BD E1 23 out EEARL,r30 = 10
D2 B6 01 23 xx in r0,DWDR = xx - byte to target
D2 BC 00 23 out EEDR,r0
D2 BB AF 23 out EECR,r26 = 04 = EEMWE
D2 BB BF 23 out EECR,r27 = 02 = EEWE

*/
