#!/usr/bin/env python3
# Trimui Smart Pro S gamepad parser: two 9600-baud MCUs (ttyS2=left, ttyS3=right),
# 10-byte frames terminated by 0x80. left axes=bytes[3,4], right axes=bytes[5,6].
import os, sys, time, select, termios, struct
from evdev import UInput, ecodes as e, AbsInfo

def open_tty(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd)
    a[0]=a[1]=a[3]=0; a[2]=termios.CS8|termios.CREAD|termios.CLOCAL
    a[4]=a[5]=termios.B9600
    a[6][termios.VMIN]=0; a[6][termios.VTIME]=0
    termios.tcsetattr(fd, termios.TCSANOW, a)
    termios.tcflush(fd, termios.TCIFLUSH)
    return fd

def frames(buf):
    out=[]; i=0
    while i+10<=len(buf):
        if buf[i+9]==0x80: out.append(buf[i:i+10]); i+=10
        else: i+=1
    return out, buf[i:]

L=open_tty('/dev/ttyS2'); R=open_tty('/dev/ttyS3')
AX=lambda: AbsInfo(0,-32767,32767,64,1024,0)
cap={e.EV_KEY:[e.BTN_SOUTH,e.BTN_EAST,e.BTN_NORTH,e.BTN_WEST,e.BTN_TL,e.BTN_TR,
               e.BTN_THUMBL,e.BTN_THUMBR,e.BTN_SELECT,e.BTN_START,
               e.BTN_DPAD_UP,e.BTN_DPAD_DOWN,e.BTN_DPAD_LEFT,e.BTN_DPAD_RIGHT],
     e.EV_ABS:[(e.ABS_X,AX()),(e.ABS_Y,AX()),(e.ABS_RX,AX()),(e.ABS_RY,AX())]}
ui=UInput(cap,name="Trimui Smart Pro S Gamepad",vendor=0x1e71,product=0x5050)
print("uinput pad created:",ui.device.path)

bufL=b''; bufR=b''; cen={}; ax={'X':0,'Y':0,'RX':0,'RY':0}
lastL=None; lastR=None
def scale(raw,c): return max(-32767,min(32767,int((raw-c)*400)))
t0=time.time(); tp=0; calib=[]
while time.time()-t0 < 25:
    time.sleep(0.004)
    try: bufL+=os.read(L,1024)
    except (OSError,BlockingIOError): pass
    if True:
        fs,bufL=frames(bufL)
        for f in fs:
            x,y=f[3],f[4]
            if 'X' not in cen: cen['X']=x; cen['Y']=y
            ax['X']=scale(x,cen['X']); ax['Y']=scale(y,cen['Y'])
            ui.write(e.EV_ABS,e.ABS_X,ax['X']); ui.write(e.EV_ABS,e.ABS_Y,ax['Y'])
            # button-candidate bytes for left: b0,b1,b5,b6
            sig=(f[0]&0x10,f[1],f[5]&0x40,f[6])
            if sig!=lastL and time.time()-t0>1: print("L btn-bytes:",("%02x %02x %02x %02x"%(f[0],f[1],f[5],f[6]))); lastL=sig
    try: bufR+=os.read(R,1024)
    except (OSError,BlockingIOError): pass
    if True:
        fs,bufR=frames(bufR)
        for f in fs:
            rx,ry=f[5],f[6]
            if 'RX' not in cen: cen['RX']=rx; cen['RY']=ry
            ax['RX']=scale(rx,cen['RX']); ax['RY']=scale(ry,cen['RY'])
            ui.write(e.EV_ABS,e.ABS_RX,ax['RX']); ui.write(e.EV_ABS,e.ABS_RY,ax['RY'])
            sig=(f[0],f[1],f[4],f[7])
            if sig!=lastR and time.time()-t0>1: print("R btn-bytes:",("%02x %02x %02x %02x"%(f[0],f[1],f[4],f[7]))); lastR=sig
    ui.syn()
    if time.time()-tp>0.5:
        print("axes L(%+6d,%+6d) R(%+6d,%+6d)"%(ax['X'],ax['Y'],ax['RX'],ax['RY'])); tp=time.time()
ui.close(); print("done")
