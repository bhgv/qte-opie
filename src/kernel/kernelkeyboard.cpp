/*

 Copyright (C) 2003             Chris Larson
 Copyright (C)       2004, 2005 Holger Hans Peter Freyther
 All rights reserved.

 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are met:

   Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.

   Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   Neither the name Chris Larson nor the names of its
   contributors may be used to endorse or promote products derived
   from this software without specific prior written permission.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 POSSIBILITY OF SUCH DAMAGE.

*/

/*
 Copyright (C) 2005 ROAD GmbH

 This program is free software; you can redistribute it
 and/or modify it under the terms of the GNU General Public
 License as published by the Free Software Foundation;
 either version 2 of the License, or (at your option) any
 later version.

 This program is distributed in the hope that it will be
 useful, but WITHOUT ANY WARRANTY; without even the implied
 warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 PURPOSE. See the GNU General Public License for more
 details.

 You should have received a copy of the GNU General Public
 License along with this program; if not, write to the Free
 Software Foundation, Inc., 59 Temple Place, Suite 330, 
 Boston, MA 02111-1307 USA


 Changes Done:
 	-ShiftL and ShiftR modifier handling
	-Keypad handling improved
	-Handle unmapped keys by lookin them up in the first column
	-Handle AutoRepeat
	-Handle Keys >127
	-Handle Increment and Decrement the Console
	-Handle the Home Key
	-Fix handling of Function Keys
	-Fix handling of the unicode value (to fix QPopupMenu,QAction)
	
 */

/*
    Paul Sokolovsky, 2007-04:
    
    1. Added support for nicely named and extensible key bindings.
       E.g., in loadkeys do:
         keycode 155 = F100
         string F100 = "power"
       where 155 is keycode a power button happens to have on your machine, F100 is
       arbitrary function key (please use F100-F120), and "power" is OPIE binding.
       Currently defined bindings are "power", "backlight", "record"
    2. K_DO ("Do") keycode is mapped to OPIE power button by default. Kernel uses
       K_DO for KEY_POWER input subsystem keycode. So, if your device does the right
       thing, it will work out of the box.
    3. Implemented NumLock handling for numeric keypad.
 */

/*
  This is an alternative implementation of the QWSTtyKeyboardHandler
  of Trolltech's QtE.

  Instead of using a hardcoded incomplete map, this implementation
  imports the Keymap from a Linux Kernel.

 */


//
// Tty keyboard
//
#include "keyboard_linux_to_qt.h"

QWSTtyKeyboardHandler::QWSTtyKeyboardHandler(const QString& device)
    : current_map(0), modifier( 0 ), numlock( false ), capslock( false )
{
    restoreLeds();
    kbdFD=open(device.isEmpty() ? "/dev/tty0" : device.latin1(), O_RDWR | O_NDELAY, 0);

    if ( kbdFD >= 0 ) {
	QSocketNotifier *notifier;
	notifier = new QSocketNotifier( kbdFD, QSocketNotifier::Read, this );
	connect( notifier, SIGNAL(activated(int)),this,
		 SLOT(readKeyboardData()) );

	// save for restore.
	tcgetattr( kbdFD, &origTermData );

	struct termios termdata;
	tcgetattr( kbdFD, &termdata );

#if !defined(_OS_FREEBSD_) && !defined(_OS_SOLARIS_)
        ioctl(kbdFD, KDSKBMODE, K_MEDIUMRAW);
#endif

	termdata.c_iflag = (IGNPAR | IGNBRK) & (~PARMRK) & (~ISTRIP);
	termdata.c_oflag = 0;
	termdata.c_cflag = CREAD | CS8;
	termdata.c_lflag = 0;
	termdata.c_cc[VTIME]=0;
	termdata.c_cc[VMIN]=1;
	cfsetispeed(&termdata, 9600);
	cfsetospeed(&termdata, 9600);
	tcsetattr(kbdFD, TCSANOW, &termdata);

	readUnicodeMap();
	readKeyboardMap();

	signal(VTSWITCHSIG, vtSwitchHandler);

#if !defined(_OS_FREEBSD_) && !defined(_OS_SOLARIS_)
	struct vt_mode vtMode;
	ioctl(kbdFD, VT_GETMODE, &vtMode);

	// let us control VT switching
	vtMode.mode = VT_PROCESS;
	vtMode.relsig = VTSWITCHSIG;
	vtMode.acqsig = VTSWITCHSIG;
	ioctl(kbdFD, VT_SETMODE, &vtMode);

	struct vt_stat vtStat;
	ioctl(kbdFD, VT_GETSTATE, &vtStat);
	vtQws = vtStat.v_active;
#endif
    }
}

QWSTtyKeyboardHandler::~QWSTtyKeyboardHandler()
{
    restoreLeds();
    if (kbdFD >= 0)
    {
#if !defined(_OS_FREEBSD_) && !defined(_OS_SOLARIS_)
        struct vt_mode vtMode;
        ioctl(kbdFD, VT_GETMODE, &vtMode);

        /* Mickey says: "Better give up control of VT switching.
         *               Hey, I really hate that OS-will-reacquire-resources on process-death
         *               kind of thinking!
         */
        vtMode.mode = VT_AUTO;
        vtMode.relsig = 0;
        vtMode.acqsig = 0;
        ioctl(kbdFD, VT_SETMODE, &vtMode);

        signal(VTSWITCHSIG, 0);
        qDebug( "~QWSTtyKeyboardHandler() - released VT." );
#endif

#if !defined(_OS_FREEBSD_) && !defined(_OS_SOLARIS_)
	ioctl(kbdFD, KDSKBMODE, K_XLATE);
#endif
	tcsetattr(kbdFD, TCSANOW, &origTermData);
	::close(kbdFD);
	kbdFD = -1;
    }
}
void QWSTtyKeyboardHandler::readUnicodeMap()
{
    if (kbdFD < 0)
        return;
    if (ioctl(kbdFD,GIO_UNISCRNMAP,acm) != 0)
        return;
}

static Qt::Key getSpecialKey(int fKey)
{
    struct kbsentry kbs;
    kbs.kb_func = fKey;

    if (ioctl(kbdFD, KDGKBSENT, &kbs) != 0)
	return Qt::Key_unknown;
    const char *str = (const char *)kbs.kb_string;

    if (!strcmp("record", str))
        return Qt::Key_F24;
    else if (!strcmp("power", str))
        return Qt::Key_F34;
    else if (!strcmp("backlight", str))
        return Qt::Key_F35;

    return Qt::Key_unknown;
}

void QWSTtyKeyboardHandler::readKeyboardMap()
{
    struct kbentry  kbe;
    if (kbdFD < 0)
        return;

    for (int map = 0; map < (1<<KG_CAPSSHIFT); ++map) {
        unsigned short kval;
        kbe.kb_table = map;

        for (int key = 0; key < NR_KEYS; ++key) {
            kbe.kb_index = key;
	    
	    
	    if ( (ioctl(kbdFD, KDGKBENT, &kbe) != 0) ||
			    ((kbe.kb_value == K_HOLE) || (kbe.kb_value == K_NOSUCHMAP)) ) {
		    kernel_map[map][key] = KeyMap( KeyMap::Key_NotMapped, 0 );
		    continue;
	    }

            kval = KVAL(kbe.kb_value);
            switch (KTYP(kbe.kb_value)) {
            /*
             * Map asciis and letters to Qt KeyCodes
             * via the map (0-255)
             */
            case KT_LETTER:
            case KT_LATIN:
            case KT_META:
                kernel_map[map][key] = KeyMap( linux_to_qt[kval], kval );
                break;

            /*
             * Handle the F Keys and map them
             * to Qt
             */
            case KT_FN:
                if ( kval <= 19 )
                    kernel_map[map][key] = KeyMap( static_cast<Qt::Key>( Qt::Key_F1  + kval ), kval );
                else if ( kval >= 30 && kval <= 44)
                    kernel_map[map][key] = KeyMap( static_cast<Qt::Key>( Qt::Key_F21 + (kval - 30) ), kval );
                else {
		    Qt::Key specialKey = getSpecialKey(kval);
		    if (specialKey != Qt::Key_unknown) {
                        kernel_map[map][key] = KeyMap( specialKey, kval );
		    } 
		    else
                    switch(kbe.kb_value ) {
                    case K_INSERT:
                        kernel_map[map][key] = KeyMap( Qt::Key_Insert, kval );
                        break;
                    case K_REMOVE:
                        kernel_map[map][key] = KeyMap( Qt::Key_Delete, kval ); // right?
                        break;
                    case K_SELECT:
                        kernel_map[map][key] = KeyMap( Qt::Key_End , kval );
                        break;
                    case K_PGUP:
                        kernel_map[map][key] = KeyMap( Qt::Key_Prior, kval );
                        break;
                    case K_PGDN:
                        kernel_map[map][key] = KeyMap( Qt::Key_Next, kval );
                        break;
                    case K_MACRO:
                        kernel_map[map][key] = KeyMap( Qt::Key_Menu, kval );
                        break;
                    case K_HELP:
                        kernel_map[map][key] = KeyMap( Qt::Key_Help, kval );
                        break;
                    case K_PAUSE:
                        kernel_map[map][key] = KeyMap( Qt::Key_Pause, kval );
                        break;
                    case K_FIND:
			kernel_map[map][key] = KeyMap( Qt::Key_Home, kval );
			break;
                    case K_DO:
			kernel_map[map][key] = KeyMap( Qt::Key_F34, kval );
			break;
                    default:
                        kernel_map[map][key] = KeyMap( Qt::Key_unknown, kval );
                        break;
                    }
		}
                break;

            case KT_SPEC:
                switch ( kbe.kb_value ) {
                case K_ENTER:
                    kernel_map[map][key] = KeyMap( Qt::Key_Enter, kval );
                    break;
                case K_CAPS:
                    kernel_map[map][key] = KeyMap( Qt::Key_CapsLock, kval );
                    break;
                case K_NUM:
                    kernel_map[map][key] = KeyMap( Qt::Key_NumLock, kval );
                    break;
                case K_HOLD:
                    kernel_map[map][key] = KeyMap( Qt::Key_ScrollLock, kval );
                    break;
		case K_DECRCONSOLE:
		    kernel_map[map][key] = KeyMap( KeyMap::Key_DecConsole, kval );
		    break;
		case K_INCRCONSOLE:
		    kernel_map[map][key] = KeyMap( KeyMap::Key_IncConsole, kval );
		    break;
		case K_HOLE:
                case K_SH_REGS:
                case K_SH_MEM:
                case K_SH_STAT:
                case K_BREAK:
                case K_CONS:
                case K_SCROLLFORW:
                case K_SCROLLBACK:
                case K_BOOT:
                case K_CAPSON:
                case K_COMPOSE:
                case K_SAK:
                case K_SPAWNCONSOLE:
                case K_BARENUMLOCK:
                default:
                    kernel_map[map][key] = KeyMap( Qt::Key_unknown, kval );
                    break;
                }
                break;
            case KT_PAD:
                /*
                 * Number Values might be wrong
                 */
                switch(kbe.kb_value ) {
                case K_P0:
                    kernel_map[map][key] = KeyMap( Qt::Key_0, kbe.kb_value );
                    break;
                case K_P1:
                    kernel_map[map][key] = KeyMap( Qt::Key_1, kbe.kb_value );
                    break;
                case K_P2:
                    kernel_map[map][key] = KeyMap( Qt::Key_2, kbe.kb_value );
                    break;
                case K_P3:
                    kernel_map[map][key] = KeyMap( Qt::Key_3, kbe.kb_value );
                    break;
                case K_P4:
                    kernel_map[map][key] = KeyMap( Qt::Key_4, kbe.kb_value );
                    break;
                case K_P5:
                    kernel_map[map][key] = KeyMap( Qt::Key_5, kbe.kb_value );
                    break;
                case K_P6:
                    kernel_map[map][key] = KeyMap( Qt::Key_6, kbe.kb_value );
                    break;
                case K_P7:
                    kernel_map[map][key] = KeyMap( Qt::Key_7, kbe.kb_value );
                    break;
                case K_P8:
                    kernel_map[map][key] = KeyMap( Qt::Key_8, kbe.kb_value );
                    break;
                case K_P9:
                    kernel_map[map][key] = KeyMap( Qt::Key_9, kbe.kb_value );
                    break;
                case K_PPLUS:
                    kernel_map[map][key] = KeyMap( Qt::Key_Plus, '+' );
                    break;
                case K_PMINUS:
                    kernel_map[map][key] = KeyMap( Qt::Key_Minus, '-' );
                    break;
                case K_PSTAR:
                    kernel_map[map][key] = KeyMap( Qt::Key_multiply, '*' );
                    break;
                case K_PSLASH:
                    kernel_map[map][key] = KeyMap( Qt::Key_division, '/' );
                    break;
                case K_PENTER:
                    kernel_map[map][key] = KeyMap( Qt::Key_Enter, kval );
                    break;
                case K_PCOMMA:
                    kernel_map[map][key] = KeyMap( Qt::Key_Comma, '.' ) ;
                    break;
                case K_PPLUSMINUS:
                    kernel_map[map][key] = KeyMap( Qt::Key_plusminus, kval );
                    break;
                case K_PDOT:
                    kernel_map[map][key] = KeyMap( Qt::Key_Comma, '.' ) ;
                    break;
                case K_PPARENL:
                    kernel_map[map][key] = KeyMap( Qt::Key_ParenLeft, kval );
                    break;
                case K_PPARENR:
                    kernel_map[map][key] = KeyMap( Qt::Key_ParenRight, kval );
                    break;
                default:
                    kernel_map[map][key] = KeyMap( Qt::Key_unknown, kval );
                    break;
                }
                break;
            case KT_DEAD:
                switch(kbe.kb_value ) {
                case K_DGRAVE:
                case K_DACUTE:
                case K_DCIRCM:
                case K_DTILDE:
                case K_DDIERE:
                case K_DCEDIL:
                default:
                    kernel_map[map][key] = KeyMap( Qt::Key_unknown, kval );
                    break;
                }
                break;

	        /*
	         * Console keys
                 */
	        case KT_CONS:
		        if ( kval < 10 )
		            kernel_map[map][key] = KeyMap(static_cast<KeyMap::ExtraKey>( KeyMap::Key_Console1+kval ), kval );
		        else
			        kernel_map[map][key] = KeyMap( Qt::Key_unknown, kval );
                break;

            case KT_CUR:
                switch(kbe.kb_value ) {
                case K_DOWN:
                    kernel_map[map][key] = KeyMap( Qt::Key_Down, kval );
                    break;
                case K_LEFT:
                    kernel_map[map][key] = KeyMap( Qt::Key_Left, kval );
                    break;
                case K_RIGHT:
                    kernel_map[map][key] = KeyMap( Qt::Key_Right, kval );
                    break;
                case K_UP:
                    kernel_map[map][key] = KeyMap( Qt::Key_Up, kval );
                    break;
                }
                break;

            case KT_SHIFT:
                switch( kbe.kb_value ) {
                case K_SHIFT:
                    kernel_map[map][key] = KeyMap( Qt::Key_Shift, kval );
                    break;
                case K_ALT:
                    kernel_map[map][key] = KeyMap( Qt::Key_Alt, kval );
                    break;
                case K_CTRL:
                    kernel_map[map][key] = KeyMap( Qt::Key_Control, kval );
                    break;
                case K_ALTGR:
                    kernel_map[map][key] = KeyMap( KeyMap::Key_AltGr, kval );
                    break;
                case K_SHIFTL:
		    kernel_map[map][key] = KeyMap( KeyMap::Key_ShiftL, kval );
		    break;
                case K_SHIFTR:
                    kernel_map[map][key] = KeyMap( KeyMap::Key_ShiftR, kval );
                    break;
		case K_CTRLL:
                case K_CTRLR:
                case K_CAPSSHIFT:
                default:
                    kernel_map[map][key] = KeyMap( Qt::Key_unknown, kval );
                    break;
                }
                break;
            /*
             * What is this for?
             */
            case KT_ASCII:
            case KT_LOCK:
            case KT_SLOCK:
            default:
                kernel_map[map][key] = KeyMap( Qt::Key_unknown, kval );
                //qWarning("keycode %d, map %d, type %d, val %d, acm %c\n", key, map, KTYP(kbe.kb_value), kval, acm[kval]);
                break;
            }
        }
    }
}
int QWSTtyKeyboardHandler::map_to_modif()
{
    int modifiers = 0;

    if (current_map & (1<<KG_ALT))
        modifiers |= Qt::AltButton;
    else if (current_map & (1<<KG_CTRL))
        modifiers |= Qt::ControlButton;
    else if (current_map & (1<<KG_SHIFT))
        modifiers |= Qt::ShiftButton;

    return modifiers;
}

/*
 * Handle Extra Keys for VT switching and Quitting
 */
void QWSTtyKeyboardHandler::handleExtra( unsigned int key, bool release ) {
    if ( !release ) {
        int term = 0;
        if ( (modifier & (1<<KG_ALT)) && (modifier & (1<<KG_CTRL)) ) {
            if ( key == Qt::Key_Left || key == KeyMap::Key_DecConsole )
                term = QMAX(vtQws -1, 1 );
            else if ( key == Qt::Key_Right || key == KeyMap::Key_IncConsole )
                term = QMIN(vtQws +1, 12 );
        }

        if ( key >= KeyMap::Key_Console1 && key <= KeyMap::Key_Console12 )
            term = key - KeyMap::Key_Console1 + 1;

        if ( term != 0 ) {
            current_map = modifier = 0;
            numlock = capslock = false;
            ioctl(kbdFD, VT_ACTIVATE, term );
            return;
        }
    }

    if ( (modifier & (1<<KG_ALT)) && (modifier & (1<<KG_CTRL) ) )
         if ( key == Qt::Key_Delete || key == Qt::Key_Backspace ) {
             qWarning( "Instructed to quit on %d", key );
             qApp->quit();
         }
}

/*
 * apply modifier
 */
void QWSTtyKeyboardHandler::modifyModifier( int map, int modify, bool release ) {
    if (map != -1) {
        if (release)
            current_map &= ~map;
        else
            current_map |= map;
    }

    if ( modify != -1 ) {
        if (release)
            modifier &= ~modify;
        else
            modifier |=  modify;
    }
}

static Qt::Key numpad2cursor[NR_PAD] = {
    Qt::Key_Insert,
    Qt::Key_End,
    Qt::Key_Down,
    Qt::Key_Next,
    Qt::Key_Left,
    Qt::Key_5,
    Qt::Key_Right,
    Qt::Key_Home,
    Qt::Key_Up,
    Qt::Key_Prior,
};

void QWSTtyKeyboardHandler::handleKey(unsigned int code, bool release)
{
    int old_modifier = modifier;
    bool mod_key = true;

    KeyMap key_map = kernel_map[current_map][code];
    if( key_map.key == KeyMap::Key_NotMapped ) {
	    qWarning("Unmapped Key Pressed fixing up map:%d modif:%d code:%d", current_map, modifier, code);
	    key_map = kernel_map[0][code];
    }
    
    unsigned short unicode = 0xffff;
    if (key_map.code < 0x100)
	unicode = acm[key_map.code & 0xff] & 0xff;
    unsigned int   qtKeyCode = key_map.key;

//    if ( !release )
//        qWarning( "KeyCode: %d KVAL: %d", qtKeyCode, key_map.code );
//        qWarning( "Alt:%d Ctrl:%d Shift:%d Key = %d", modifier & (1<<KG_ALT),
//                  modifier & (1<<KG_CTRL),
//                  modifier & (1<<KG_SHIFT), key_map.key );
//    qDebug("code %d, mCode %d, uni '%c', qtKeyCode %d", code, map.code,
//           QChar(unicode ).isPrint() ?
//           unicode : '?' , qtKeyCode);

    // Handle map changes based on press/release of modifiers
    // hardcoded for now
    int modif = -1;
    int map   = -1;
    bool lock = false;
    switch (qtKeyCode)
    {
    case Qt::Key_Alt:
        unicode = 0xffff;
        modif = (1<<KG_ALT);
	map = modif;
        break;
    case Qt::Key_Control:
        unicode = 0xffff;
        modif = (1<<KG_CTRL);
        map   = modif;
        break;
    case Qt::Key_Shift:
        unicode = 0xffff;
        modif = (1<<KG_SHIFT);
        map   = modif;
        break;
    case KeyMap::Key_AltGr:
        map   = (1<<KG_ALTGR );
        break;
    case KeyMap::Key_ShiftL:
        unicode = 0xfff;
	map   = (1<<KG_SHIFTL);
	break;
    case KeyMap::Key_ShiftR:
        unicode = 0xfff;
	map   = (1<<KG_SHIFTR);
	break;
    case Qt::Key_Left:
    case Qt::Key_Right:
    case Qt::Key_Up:
    case Qt::Key_Down:
        unicode = 0xffff;
        mod_key = false;
#if 0
        if (qt_screen->isTransformed())
            qtKeyCode = static_cast<Qt::Key>( xform_dirkey(static_cast<int>( qtKeyCode ) ) );
#endif
        break;
    /*
     * handle lock, we don't handle scroll lock!
     */
    case Qt::Key_CapsLock:
    case Qt::Key_NumLock:
        unicode = 0xffff;
        lock = true;
    default:
        mod_key = false;
        break;
    }


    /*
     * Change the Map. We handle locks a bit different
     */
    if ( lock )
        modifyLock( qtKeyCode, release );
    else
        modifyModifier( map, modif, release );

    handleExtra( qtKeyCode, release );

    // Do NumLock
    if (KTYP(key_map.code) == KT_PAD) {
	if (!numlock) {
	    qtKeyCode = numpad2cursor[KVAL(key_map.code)];
	} else {
    	    unicode = KVAL(key_map.code) + '0';
        }
    }

    /*
     * do not repeat modifier keys
     */
    if ( modifier == old_modifier && mod_key )
        return;

    processKeyEvent(unicode, qtKeyCode, map_to_modif(), !release, 0);
}


/*
 * We will read the keys off the kernel. We have two cases here
 * 1. ) keycode < 128, bit8 is the down/up bit and the rest is
 *      is the value of the key. we can simply process it
 * 2. ) keycode > 128, the first byte is either empty or 0x80.
 *      We need to save the status ( press/release ) the following
 *      two bytes.
 *      The difficulty is we might have not read all keys into the
 *      buffer. This makes the reading of highkeys a bit harder and
 *      I've decided against rereading, or adding a special case for
 *      the 'all' buffers in loop instead we have a simple state machine.
 */
void QWSTtyKeyboardHandler::readKeyboardData()
{
    unsigned char buf[81];
    unsigned char code;
    bool     release = false;
    bool     release_bit;

    bool     highKey = false;
    unsigned int highKeyCode = 0;
    unsigned int highNeedMoreKey = 0;

    int n = ::read(kbdFD, buf, 80 );
    for ( int loop = 0; loop < n; loop++ ) {
        code = buf[loop] & 0x7f;
        release_bit = buf[loop] & 0x80;

        if ( highKey ) {
            if ( highNeedMoreKey == 2 ) {
                highNeedMoreKey--;
                highKeyCode = code << 7;
            }else if ( highNeedMoreKey == 1 ) {
                highNeedMoreKey = 0;
                highKeyCode |= code;
                highKey = false;
                if ( highKeyCode > 127 && highKeyCode < NR_KEYS )
                   handleKey( highKeyCode, release );
            }
        }else if (code == 0) {
            highKey = true;
            highNeedMoreKey = 2;
            release = release_bit;
        }else {
            release = release_bit;
            handleKey(code, release);
        }
    }
}

void QWSTtyKeyboardHandler::modifyLock( unsigned int lock, bool release ) {
    if ( !release )
        return;

    if ( lock == Qt::Key_CapsLock ) {
        toggleLed( LED_CAP );
        capslock = !capslock;
    }else if ( lock == Qt::Key_NumLock ) {
        toggleLed( LED_NUM );
        numlock = !numlock;
    }
}

void QWSTtyKeyboardHandler::restoreLeds() {
    unsigned int leds;
    ioctl(kbdFD, KDGETLED, &leds );
    leds &= ~LED_CAP;
    leds &= ~LED_NUM;
    ioctl(kbdFD, KDSETLED, &leds );
}

void QWSTtyKeyboardHandler::toggleLed(unsigned int led) {
    unsigned int leds;
    int ret = ioctl(kbdFD, KDGETLED, &leds );
    leds = leds & led ? (leds & ~led) : (leds | led);
    ret = ioctl(kbdFD, KDSETLED, &leds );
}

void QWSTtyKeyboardHandler::processKeyEvent(int unicode, int keycode, int modifiers, bool isPress, bool autoRepeat ) {
    static int last_unicode = -1;
    static int last_keycode = -1;

    autoRepeat = false;
    if( last_unicode == unicode && last_keycode == keycode && isPress )
        autoRepeat = true;

    QWSPC101KeyboardHandler::processKeyEvent(unicode, keycode, modifiers, isPress, autoRepeat);

    if ( isPress ) {
        last_unicode = unicode;
        last_keycode = keycode;
    } else {
        last_unicode = last_keycode = -1;
    }
}
