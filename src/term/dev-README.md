### Terminal Input Mappings (VT100 / VT220 / xterm)

The interactive shell uses the standard Control Sequence Introducer (CSI) format (`ESC [` or `\x1b[`) to decode multi-byte keyboard sequences. The input processing state machine categorizes incoming codes into two distinct grammatical structures depending on the character immediately following the `[`:

#### 1. Standard 3-Byte Cursor Keys (`ESC [ Letter`)

These keys terminate immediately on the command letter byte (ASCII range `0x40` to `0x7E`). They are exactly **3 bytes long** (`0x1B`, `0x5B`, `Letter`) and do not use a trailing terminating character (tilde).

| Physical Key | Sequence | Hexadecimal Bytes | Parser Evaluation Token |
| :--- | :--- | :--- | :--- |
| **Arrow Up** | `\x1b[A` | `1B 5B 41` | `SES_KEY_ARROW_UP` |
| **Arrow Down** | `\x1b[B` | `1B 5B 42` | `SES_KEY_ARROW_DN` |
| **Arrow Right** | `\x1b[C` | `1B 5B 43` | `SES_KEY_ARROW_RT` |
| **Arrow Left** | `\x1b[D` | `1B 5B 44` | `SES_KEY_ARROW_LF` |
| **Home** (xterm) | `\x1b[H` | `1B 5B 48` | `SES_KEY_HOME` |
| **End** (xterm) | `\x1b[F` | `1B 5B 46` | `SES_KEY_END` |

#### 2. Multi-Byte Editing Keys (`ESC [ Digit ~`)

These functions are inherited from the VT220 editing keypad layout. They contain one or more numeric parameter bytes (ASCII range `0x30` to `0x3F`) and **must terminate with a tilde (`~`, 0x7E)**. They are exactly **4 bytes long**.

| Physical Key | Sequence | Hexadecimal Bytes | Parameter ID (`_esc_collected`) |
| :--- | :--- | :--- | :--- |
| **Insert** | `\x1b[1~` | `1B 5B 31 7E` | `'1'` |
| **Forward Delete**| `\x1b[3~` | `1B 5B 33 7E` | `'3'` |
| **End** (VT220) | `\x1b[4~` | `1B 5B 34 7E` | `'4'` |
| **Page Up** | `\x1b[5~` | `1B 5B 35 7E` | `'5'` |
| **Page Down** | `\x1b[6~` | `1B 5B 36 7E` | `'6'` |

#### 3. Function Keys (F-Keys)

Function keys mimic standard VT220 protocol constraints where identifiers pass variable numeric sequences terminating with a trailing tilde (`~`), with the historical exception of F1-F4 mapping to 3-byte `ESC O` Single Shift 3 (SS3) layouts inside modern terminal emulators.

| Physical Key | Sequence | Hexadecimal Bytes | Parameter ID Sequence |
| :--- | :--- | :--- | :--- |
| **F1** | `\x1bOP` | `1B 4F 50` | *xterm SS3 layout* |
| **F2** | `\x1bOQ` | `1B 4F 51` | *xterm SS3 layout* |
| **F3** | `\x1bOR` | `1B 4F 52` | *xterm SS3 layout* |
| **F4** | `\x1bOS` | `1B 4F 53` | *xterm SS3 layout* |
| **F5** | `\x1b[15~` | `1B 5B 31 35 7E` | `"15"` |
| **F6** | `\x1b[17~` | `1B 5B 31 37 7E` | `"17"` |
| **F7** | `\x1b[18~` | `1B 5B 31 38 7E` | `"18"` |
| **F8** | `\x1b[19~` | `1B 5B 31 39 7E` | `"19"` |
| **F9** | `\x1b[20~` | `1B 5B 32 30 7E` | `"20"` |
| **F10** | `\x1b[21~` | `1B 5B 32 31 7E` | `"21"` |
| **F11** | `\x1b[23~` | `1B 5B 32 33 7E` | `"23"` |
| **F12** | `\x1b[24~` | `1B 5B 32 34 7E` | `"24"` |

### Standard Control Character Key Actions (C0 / CLI Layout)

The shell handles single-byte asynchronous control signals (ASCII values `0x01` through `0x1F`, and `0x7F`) to manage command-line line editing, processing flow state alterations, and stream manipulation.

| Keystroke     | Hex Value | Control Identifier     | Standard CLI / Shell Function                                                       |
| :------------ | :-------- | :--------------------- | :---------------------------------------------------------------------------------- |
| **Ctrl + A**  | `0x01`    | SOH (Start of Heading) | **Home:** Moves the cursor to the beginning of the current line.                    |
| **Ctrl + B**  | `0x02`    | STX (Start of Text)    | **Left:** Moves the cursor one position to the left (non-destructive).              |
| **Ctrl + C**  | `0x03`    | ETX (End of Text)      | **SIGINT:** Cancels/interrupts the currently running operation or prompt.           |
| **Ctrl + D**  | `0x04`    | EOT (End of Trans.)    | **EOF / Delete:** Logs out/exits the shell, or forward-deletes the char at cursor.  |
| **Ctrl + E**  | `0x05`    | ENQ (Enquiry)          | **End:** Moves the cursor to the absolute end of the current line.                  |
| **Ctrl + F**  | `0x06`    | ACK (Acknowledge)      | **Right:** Moves the cursor one position to the right (non-destructive).            |
| **Ctrl + G**  | `0x07`    | BEL (Bell)             | **Audible/Visual Alert:** Emits an alert chime or flashes the terminal workspace.   |
| **Ctrl + H**  | `0x08`    | BS  (Backspace)        | **Backspace:** Moves cursor left; historically alternative for backward delete.     |
| **Ctrl + I**  | `0x09`    | HT  (Horizontal Tab)   | **Tab Completion:** Triggers context-aware command or path auto-completion.         |
| **Ctrl + J**  | `0x0A`    | LF  (Line Feed)        | **Execute line:** Evaluates the buffer (often treated interchangeably with CR).     |
| **Ctrl + K**  | `0x0B`    | VT  (Vertical Tab)     | **Kill Forward:** Erases all characters from the cursor position to end of line.    |
| **Ctrl + L**  | `0x0C`    | FF  (Form Feed)        | **Clear Screen:** Clears terminal viewport and repaints current prompt at top.      |
| **Ctrl + M**  | `0x0D`    | CR  (Carriage Return)  | **Execute line:** Submits the currently typed line buffer to the processor.         |
| **Ctrl + N**  | `0x0E`    | SO  (Shift Out)        | **History Next:** Recalls the next command entry down (same as Arrow Down).         |
| **Ctrl + O**  | `0x0F`    | SI  (Shift In)         | **Operate:** Executes current line entry and immediately pulls up next entry.       |
| **Ctrl + P**  | `0x10`    | DLE (Data Link Esc.)   | **History Previous:** Recalls the prior command entry up (same as Arrow Up).        |
| **Ctrl + Q**  | `0x11`    | DC1 (Device Control 1) | **XON:** Resumes streaming display transmission output after a pause condition.     |
| **Ctrl + R**  | `0x12`    | DC2 (Device Control 2) | **Reverse Search:** Starts an incremental reverse scan of the command history.      |
| **Ctrl + S**  | `0x13`    | DC3 (Device Control 3) | **XOFF:** Suspends terminal stream display output (blocks execution visually).      |
| **Ctrl + T**  | `0x14`    | DC4 (Device Control 4) | **Transpose:** Swaps the character at the cursor with the one preceding it.         |
| **Ctrl + U**  | `0x15`    | NAK (Negative Ack.)    | **Kill Line / Erase:** Deletes entire line buffer, or clears from cursor to BOL.    |
| **Ctrl + V**  | `0x16`    | SYN (Synchronous Idle) | **Literal Insert:** Makes the next entered keystroke escape literal processing.     |
| **Ctrl + W**  | `0x17`    | ETB (End of Text Blk.) | **Kill Word:** Erases characters backward from cursor up to the nearest whitespace. |
| **Ctrl + X**  | `0x18`    | CAN (Cancel)           | **Erase / Extension Prefix:** Often used to wipe line, or as multi-key prefix.      |
| **Ctrl + Y**  | `0x19`    | EM  (End of Medium)    | **Yank:** Pastes (yanks) back the last deleted block of text killed by Ctrl+K/W/U.  |
| **Ctrl + Z**  | `0x1A`    | SUB (Substitute)       | **SIGTSTP:** Suspends current job processing loop to put it in background.          |
| **Ctrl + [**  | `0x1B`    | ESC (Escape)           | **Escape Sequence Prefix:** Initiates standard ANSI multi-byte CSI string parsing.  |
| **Ctrl + \\** | `0x1C`    | FS  (File Separator)   | **SIGQUIT:** Force-terminates current process loop immediately with core dump.      |
| **Ctrl + ]**  | `0x1D`    | GS  (Group Separator)  | **Telnet Escape Character:** Standard default escape command for telnet links.      |
| **Ctrl + ^**  | `0x1E`    | RS  (Record Separator) | **Toggle / Home:** Rarely used modernly; original hardware home terminal command.   |
| **Ctrl + _**  | `0x1F`    | US  (Unit Separator)   | **Undo:** Reverts the last text editing manipulation performed on line buffer.      |
| **Backspace** | `0x7F`    | DEL (Delete)           | **Erase Left:** Standard default modern terminal value for backward deletion.       |
