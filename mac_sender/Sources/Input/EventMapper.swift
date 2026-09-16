import CoreGraphics
import Carbon.HIToolbox

/// Maps Windows virtual key codes to macOS CGKeyCode values,
/// and converts normalized coordinates to screen coordinates.
enum EventMapper {

    /// Map Windows VK code to macOS CGKeyCode.
    /// This covers the most common keys. Unmapped keys are passed through.
    static func mapKeyCode(_ windowsVK: UInt16) -> CGKeyCode {
        return windowsToMacKeyMap[windowsVK] ?? CGKeyCode(windowsVK)
    }

    /// Strict mapping used by Cross. An unknown Windows virtual key must not
    /// be reinterpreted as an unrelated macOS hardware key code.
    static func mapCrossKeyCode(_ windowsVK: UInt16, swapControlCommand: Bool) -> CGKeyCode? {
        switch windowsVK {
        case 0x10, 0xA0: return CGKeyCode(kVK_Shift)
        case 0xA1: return CGKeyCode(kVK_RightShift)
        case 0x11, 0xA2: return CGKeyCode(swapControlCommand ? kVK_Command : kVK_Control)
        case 0xA3: return CGKeyCode(swapControlCommand ? kVK_RightCommand : kVK_RightControl)
        case 0x12, 0xA4: return CGKeyCode(kVK_Option)
        case 0xA5: return CGKeyCode(kVK_RightOption)
        case 0x5B: return CGKeyCode(swapControlCommand ? kVK_Control : kVK_Command)
        case 0x5C: return CGKeyCode(swapControlCommand ? kVK_RightControl : kVK_RightCommand)
        case 0x0C, 0x90: return CGKeyCode(kVK_ANSI_KeypadClear)
        case 0x14: return CGKeyCode(kVK_CapsLock)
        case 0x2D: return CGKeyCode(kVK_Help)
        case 0x60: return CGKeyCode(kVK_ANSI_Keypad0)
        case 0x61: return CGKeyCode(kVK_ANSI_Keypad1)
        case 0x62: return CGKeyCode(kVK_ANSI_Keypad2)
        case 0x63: return CGKeyCode(kVK_ANSI_Keypad3)
        case 0x64: return CGKeyCode(kVK_ANSI_Keypad4)
        case 0x65: return CGKeyCode(kVK_ANSI_Keypad5)
        case 0x66: return CGKeyCode(kVK_ANSI_Keypad6)
        case 0x67: return CGKeyCode(kVK_ANSI_Keypad7)
        case 0x68: return CGKeyCode(kVK_ANSI_Keypad8)
        case 0x69: return CGKeyCode(kVK_ANSI_Keypad9)
        case 0x6A: return CGKeyCode(kVK_ANSI_KeypadMultiply)
        case 0x6B: return CGKeyCode(kVK_ANSI_KeypadPlus)
        case 0x6C: return CGKeyCode(kVK_ANSI_KeypadEnter)
        case 0x6D: return CGKeyCode(kVK_ANSI_KeypadMinus)
        case 0x6E: return CGKeyCode(kVK_ANSI_KeypadDecimal)
        case 0x6F: return CGKeyCode(kVK_ANSI_KeypadDivide)
        case 0x7C: return CGKeyCode(kVK_F13)
        case 0x7D: return CGKeyCode(kVK_F14)
        case 0x7E: return CGKeyCode(kVK_F15)
        case 0x7F: return CGKeyCode(kVK_F16)
        case 0x80: return CGKeyCode(kVK_F17)
        case 0x81: return CGKeyCode(kVK_F18)
        case 0x82: return CGKeyCode(kVK_F19)
        case 0x83: return CGKeyCode(kVK_F20)
        case 0xE2: return CGKeyCode(kVK_ISO_Section)
        default: return windowsToMacKeyMap[windowsVK]
        }
    }

    /// Convert normalized [0,1] coordinates to screen pixel coordinates.
    static func denormalize(x: Float, y: Float,
                            screenWidth: Int, screenHeight: Int) -> CGPoint {
        return CGPoint(
            x: CGFloat(x) * CGFloat(screenWidth),
            y: CGFloat(y) * CGFloat(screenHeight)
        )
    }

    /// Map modifier string array to CGEventFlags.
    static func mapModifiers(_ modifiers: [String]?) -> CGEventFlags {
        var flags = CGEventFlags()
        guard let mods = modifiers else { return flags }

        for mod in mods {
            switch mod {
            case "shift": flags.insert(.maskShift)
            case "ctrl":  flags.insert(.maskControl)
            case "alt":   flags.insert(.maskAlternate)
            case "meta":  flags.insert(.maskCommand)
            default: break
            }
        }
        return flags
    }

    static func mapModifiers(_ modifiers: [String]?, swapControlCommand: Bool) -> CGEventFlags {
        var flags = CGEventFlags()
        for modifier in modifiers ?? [] {
            switch modifier {
            case "shift": flags.insert(.maskShift)
            case "ctrl": flags.insert(swapControlCommand ? .maskCommand : .maskControl)
            case "alt": flags.insert(.maskAlternate)
            case "meta": flags.insert(swapControlCommand ? .maskControl : .maskCommand)
            default: break
            }
        }
        return flags
    }

    /// Map button index to mouse button and event types.
    static func mouseEventTypes(button: Int, isDown: Bool)
        -> (eventType: CGEventType, mouseButton: CGMouseButton) {
        switch button {
        case 0: // Left
            return (isDown ? .leftMouseDown : .leftMouseUp, .left)
        case 1: // Right
            return (isDown ? .rightMouseDown : .rightMouseUp, .right)
        case 2: // Middle
            return (isDown ? .otherMouseDown : .otherMouseUp, .center)
        default:
            return (isDown ? .otherMouseDown : .otherMouseUp, .center)
        }
    }

    // MARK: - Key Code Mapping Table

    /// Windows VK code -> macOS CGKeyCode
    /// Reference: https://docs.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes
    private static let windowsToMacKeyMap: [UInt16: CGKeyCode] = [
        // Letters A-Z (VK_A=0x41 .. VK_Z=0x5A)
        0x41: CGKeyCode(kVK_ANSI_A),
        0x42: CGKeyCode(kVK_ANSI_B),
        0x43: CGKeyCode(kVK_ANSI_C),
        0x44: CGKeyCode(kVK_ANSI_D),
        0x45: CGKeyCode(kVK_ANSI_E),
        0x46: CGKeyCode(kVK_ANSI_F),
        0x47: CGKeyCode(kVK_ANSI_G),
        0x48: CGKeyCode(kVK_ANSI_H),
        0x49: CGKeyCode(kVK_ANSI_I),
        0x4A: CGKeyCode(kVK_ANSI_J),
        0x4B: CGKeyCode(kVK_ANSI_K),
        0x4C: CGKeyCode(kVK_ANSI_L),
        0x4D: CGKeyCode(kVK_ANSI_M),
        0x4E: CGKeyCode(kVK_ANSI_N),
        0x4F: CGKeyCode(kVK_ANSI_O),
        0x50: CGKeyCode(kVK_ANSI_P),
        0x51: CGKeyCode(kVK_ANSI_Q),
        0x52: CGKeyCode(kVK_ANSI_R),
        0x53: CGKeyCode(kVK_ANSI_S),
        0x54: CGKeyCode(kVK_ANSI_T),
        0x55: CGKeyCode(kVK_ANSI_U),
        0x56: CGKeyCode(kVK_ANSI_V),
        0x57: CGKeyCode(kVK_ANSI_W),
        0x58: CGKeyCode(kVK_ANSI_X),
        0x59: CGKeyCode(kVK_ANSI_Y),
        0x5A: CGKeyCode(kVK_ANSI_Z),

        // Numbers 0-9 (VK_0=0x30 .. VK_9=0x39)
        0x30: CGKeyCode(kVK_ANSI_0),
        0x31: CGKeyCode(kVK_ANSI_1),
        0x32: CGKeyCode(kVK_ANSI_2),
        0x33: CGKeyCode(kVK_ANSI_3),
        0x34: CGKeyCode(kVK_ANSI_4),
        0x35: CGKeyCode(kVK_ANSI_5),
        0x36: CGKeyCode(kVK_ANSI_6),
        0x37: CGKeyCode(kVK_ANSI_7),
        0x38: CGKeyCode(kVK_ANSI_8),
        0x39: CGKeyCode(kVK_ANSI_9),

        // Function keys
        0x70: CGKeyCode(kVK_F1),
        0x71: CGKeyCode(kVK_F2),
        0x72: CGKeyCode(kVK_F3),
        0x73: CGKeyCode(kVK_F4),
        0x74: CGKeyCode(kVK_F5),
        0x75: CGKeyCode(kVK_F6),
        0x76: CGKeyCode(kVK_F7),
        0x77: CGKeyCode(kVK_F8),
        0x78: CGKeyCode(kVK_F9),
        0x79: CGKeyCode(kVK_F10),
        0x7A: CGKeyCode(kVK_F11),
        0x7B: CGKeyCode(kVK_F12),

        // Special keys
        0x0D: CGKeyCode(kVK_Return),         // VK_RETURN
        0x1B: CGKeyCode(kVK_Escape),         // VK_ESCAPE
        0x08: CGKeyCode(kVK_Delete),          // VK_BACK (Backspace)
        0x09: CGKeyCode(kVK_Tab),            // VK_TAB
        0x20: CGKeyCode(kVK_Space),          // VK_SPACE
        0x2E: CGKeyCode(kVK_ForwardDelete),  // VK_DELETE

        // Navigation
        0x25: CGKeyCode(kVK_LeftArrow),      // VK_LEFT
        0x26: CGKeyCode(kVK_UpArrow),        // VK_UP
        0x27: CGKeyCode(kVK_RightArrow),     // VK_RIGHT
        0x28: CGKeyCode(kVK_DownArrow),      // VK_DOWN
        0x24: CGKeyCode(kVK_Home),           // VK_HOME
        0x23: CGKeyCode(kVK_End),            // VK_END
        0x21: CGKeyCode(kVK_PageUp),         // VK_PRIOR (Page Up)
        0x22: CGKeyCode(kVK_PageDown),       // VK_NEXT (Page Down)

        // Modifiers (useful for tracking state)
        0x10: CGKeyCode(kVK_Shift),          // VK_SHIFT
        0x11: CGKeyCode(kVK_Control),        // VK_CONTROL
        0x12: CGKeyCode(kVK_Option),         // VK_MENU (Alt)
        0x5B: CGKeyCode(kVK_Command),        // VK_LWIN

        // Punctuation
        0xBA: CGKeyCode(kVK_ANSI_Semicolon), // VK_OEM_1 (;)
        0xBB: CGKeyCode(kVK_ANSI_Equal),     // VK_OEM_PLUS (=)
        0xBC: CGKeyCode(kVK_ANSI_Comma),     // VK_OEM_COMMA (,)
        0xBD: CGKeyCode(kVK_ANSI_Minus),     // VK_OEM_MINUS (-)
        0xBE: CGKeyCode(kVK_ANSI_Period),    // VK_OEM_PERIOD (.)
        0xBF: CGKeyCode(kVK_ANSI_Slash),     // VK_OEM_2 (/)
        0xC0: CGKeyCode(kVK_ANSI_Grave),     // VK_OEM_3 (`)
        0xDB: CGKeyCode(kVK_ANSI_LeftBracket),  // VK_OEM_4 ([)
        0xDC: CGKeyCode(kVK_ANSI_Backslash), // VK_OEM_5 (\)
        0xDD: CGKeyCode(kVK_ANSI_RightBracket), // VK_OEM_6 (])
        0xDE: CGKeyCode(kVK_ANSI_Quote),     // VK_OEM_7 (')
    ]
}
