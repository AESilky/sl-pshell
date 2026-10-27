/**
 * User Interface - On the terminal.
 *
 * Copyright 2023-26 AESilky
 * SPDX-License-Identifier: MIT License
 *
*/
#include "shell.h"

#include "cmd.h"
#include "term.h"

#include "pico/printf.h"
#include "pico/stdio.h"

#include <ctype.h>
#include <string.h>

#define ESC_NOT_IN_PROGRESS (-1)
#define ESC_CHARS_MAX 16

static bool _modinit_called;
static bool _started;
static const char* _shell_title;

/** Function to call when the stdio character ready interrupt handler indicates there is a character ready */
static shell_notify_fn _notify_of_char_rdy;

static term_color_t _color_term_text_current_bg;
static term_color_t _color_term_text_current_fg;

static shell_control_char_handler _control_char_handler[32]; // Room for a handler for each control character

static shell_escape_seq_handler _escseq_handler[_SEH_NUM]; // Room for the ESC Sequences that we support

static char _line_buf[SHELL_GETLINE_MAX_LEN];
static int16_t _line_len;   // Total characters currently in the buffer
static int16_t _cursor_ndx; // Current logical insertion/deletion point (0 to line_len)

// static char _getline_buf[SHELL_GETLINE_MAX_LEN];
// static int16_t _getline_index;

static int _esc_collecting; // If -1, not collecting. Else, index to store the next received character until done.
static char _esc_collected[ESC_CHARS_MAX + 1]; // Room to collect characters for an escape sequence.

static bool _host_connected;
static bool _host_welcomed;

static bool _wraptext_on;
static int _wraptext_column;
static char _wraptext_line[2 * shell_COLUMNS];

static shell_input_available_handler _input_available_handler;
static shell_getline_callback_fn _getline_callback; // Function pointer to be called when an input line is ready

static void _host_welcome();
static bool _lb_append(char c);
static bool _lb_shift_right();
static bool _process_char(char c, bool process_ctrl);

// Helper to refresh the terminal view inline from the current cursor position forward
void _refresh_linert_from_cursor(bool advance_cur) {
    // Save current physical position by writing out remaining characters
    for (int i = _cursor_ndx; i < _line_len; i++) {
        putchar(_line_buf[i]);
    }
    // Erase any leftover visual characters at the end of the terminal line
    term_erase_eol();
    // Move terminal cursor physically back to its proper position
    int cadj = (advance_cur ? 1 : 0);
    int move_back = _line_len - (_cursor_ndx + cadj);
    if (move_back > 0) {
        term_cursor_left(move_back);
    }
}

/**
 * @brief Add a character at the cursor location (within line or append) and display.
 * 
 * @param c Character to add 
 * @return bool true: Character added. false: Unable to add character (probably line full).
 */
static bool _lb_add_at_cursor(char c) {
    bool retval = true;
    if (_cursor_ndx == _line_len) {
        // Adding to the end, use append.
        if (_lb_append(c)) {
            putchar(c);
            goto FINALLY_;
        }
        retval = false;
        goto FINALLY_;
    }
    else {
        // Adding within the line
        _lb_shift_right();
        _line_buf[_cursor_ndx] = c;
        _refresh_linert_from_cursor(true);
        _cursor_ndx++;
    }
FINALLY_:
    return (retval);
}

/** 
 * @brief Append a char to the line buffer if possible
 * 
 * After appending, updates _line_len and sets _cursor_ndx to _line_len.
 * 
 * @return true: Character appended. false: EOL, character cannot be appended.
 * 
 */
static bool _lb_append(char c) {
    if (!(_line_len < (SHELL_GETLINE_MAX_LEN - 1))) {
        return false; // At the end of the line, can't append
    }
    _line_buf[_line_len++] = c;
    _cursor_ndx = _line_len;
    return true;
}

/**
 * @brief Shift the _line_buf content from _cursor_ndx to _line_len left `n` positions
 * 
 * Shifts the line content left by `n` characters up to the _cursor_inx.
 * @param n The number of left character shifts to perform
 * @return The number shifted (if less than `n` there weren't enough positions to the left)
 */
static int16_t _lb_shift_left(int16_t n) {
    int16_t lfshft = (n <= _cursor_ndx ? n : _cursor_ndx);
    int16_t f = _cursor_ndx;
    int16_t t = _cursor_ndx - lfshft;
    int16_t sc = _line_len - _cursor_ndx;
    // Shift remaining trailing data left over the old character spots
    memmove(&_line_buf[t], &_line_buf[f], sc);
    _cursor_ndx -= lfshft;
    _line_len -= lfshft;
    return lfshft;
}

/**
 * @brief Shift the _line_buf content from _cursor_ndx to _line_len right
 *
 * Shifts the line content right by 1 character if the line length will accept it.
 * @return true: Line was shifted. False: Line couldn't be shifted (line is max length).
 */
static bool _lb_shift_right() {
    if (!(_line_len < (SHELL_GETLINE_MAX_LEN - 1))) {
        return false; // The line is full, cannot shift
    }
    int16_t f = _cursor_ndx;
    int16_t t = f+1;
    int16_t sc = (_line_len - _cursor_ndx);
    // Shift the characters after the cursor
    memmove(&_line_buf[t], &_line_buf[f], sc);
    _line_len++;
    return true;
}

static void _do_backspace() {
    if (_lb_shift_left(1)) {
        term_delete_char_left();
    }
}

static void _do_delatcur() {
    // Check that we aren't at the end of the line
    if (_cursor_ndx < _line_len) {
        _cursor_ndx++;
        _lb_shift_left(1);
        term_delete_char_at_cursor();
    }
}

/**
 * @brief Init/ReInit the terminal (connected)
 * @ingroup app
 */
static void _handle_term_init() {
    _host_welcome();
}


/**
 * @brief A `term_notify_on_input_fn` handler for input ready.
 * @ingroup shell
 */
void _input_ready_hook(void) {
    // Since this is called by an interrupt handler,
    // call the message poster if one was supplied.
    _notify_of_char_rdy();

    // The hook is cleared on notify, so hook ourself back in.
    term_register_notify_on_input(_input_ready_hook);
}

static shell_control_char_handler _get_control_char_handler(char c) {
    if (iscntrl(c)) {
        return _control_char_handler[(int)c];
    }
    return (NULL);
}

static shell_escape_seq_handler _get_escseq_handler(sescseq_t escseq) {
    return _escseq_handler[escseq];
}

static void _getline_continue() {
    int ci;

    // Process characters that are available.
    while ((ci = term_getc()) >= 0) {
        if (!_host_welcomed) {
            _host_welcome();
            continue;
        }
        char c = (char)ci;
        if (!_process_char(c, true)) {
            // See if there is a handler registered for this, else BEEP
            if (!shell_handle_control_character(c)) {
                // Control or 8-bit character we don't deal with
                putchar(BEL);
                _esc_collecting = ESC_NOT_IN_PROGRESS;
            }
        }
        // `while` will see if there are more chars available
    }
    // No more input chars are available, but we haven't gotten EOL yet,
    // hook for more to wake back up...
    term_register_notify_on_input(_input_ready_hook);
}

static bool _ehandle_cur_end(sescseq_t escseq, const char* escstr) {
    shell_getline_end();
    return true;
}

static bool _ehandle_cur_home(sescseq_t escseq, const char* escstr) {
    shell_getline_home();
    return true;
}

static bool _ehandle_cur_left(sescseq_t escseq, const char* escstr) {
    if (_cursor_ndx > 0) {
        _cursor_ndx--;
        term_cursor_left_1();
    }
    return true;
}

static bool _ehandle_cur_right(sescseq_t escseq, const char* escstr) {
    if (_cursor_ndx < _line_len) {
        _cursor_ndx++;
        term_cursor_right_1();
    }
    return true;
}

static bool _ehandle_delatcur(sescseq_t escseq, const char* escstr) {
    _do_delatcur();
    return true;
}

static void _handle_backspace(char c) {
    _do_backspace();
}

static void _handle_cancel_line(char c) {
    // ^X CAN (Cancel) - Erases the current input
    if (_cursor_ndx > 0) {
        term_cursor_left(_cursor_ndx);
        term_erase_eol();
    }
    _cursor_ndx = 0;
    _line_len = 0;
}

static void _handle_delatcur(char c) {
    _do_delatcur();
}

static void _host_welcome() {
    // Now to a full init of the terminal
    term_init();
    term_set_title(_shell_title);
    term_text_normal();
    // Tell the Host hello
    shell_puts(_shell_title);
    _host_welcomed = true;
    _started = true;
    shell_build();
    cmd_activate(true);
}

static bool _process_char(char c, bool process_ctrl) {
    bool processed = false;
    shell_getline_callback_fn handle_line = _getline_callback;

    if (process_ctrl) {
        // See if we are processing an ESC sequence.
        if (_esc_collecting != ESC_NOT_IN_PROGRESS) {
            // Yes...
            if (_esc_collecting == 0) {
                // We are looking for CSI, see if this is it.
                if ('[' == c) {
                    _esc_collected[_esc_collecting++] = c;
                    _esc_collected[_esc_collecting] = '\0';
                    processed = true;
                    goto FINALLY_;
                }
                else {
                    _esc_collecting = ESC_NOT_IN_PROGRESS;
                    goto PROC_AS_PLAIN_;
                }
            }
            else {
                // We are into the sequence, see if we have collected the whole thing.
                //
                int escndx = _esc_collecting;
                shell_escape_seq_handler fn = NULL;
                _esc_collected[_esc_collecting++] = c;
                _esc_collected[_esc_collecting] = '\0';
                //  If c is an uppercase character it marks the end of the sequence.
                //  See if it is one we handle.
                if (c >= 'A' && c <= 'Z') {
                    _esc_collecting = ESC_NOT_IN_PROGRESS;
                    // The uppercase character sequences we handle are all just after the CSI
                    if (escndx == 1) {
                        // 'Up Arrow' "CSI A"
                        if ('A' == c) {
                            fn = _get_escseq_handler(SES_KEY_ARROW_UP);
                            if (fn) {
                                processed = fn(SES_KEY_ARROW_UP, _esc_collected);
                            }
                            goto ESC_FINISH_;
                        }
                        // 'Down Arrow' "CSI B"
                        if ('B' == c) {
                            fn = _get_escseq_handler(SES_KEY_ARROW_DN);
                            if (fn) {
                                processed = fn(SES_KEY_ARROW_DN, _esc_collected);
                            }
                            goto ESC_FINISH_;
                        }
                        // 'Right Arrow' "CSI C"
                        if ('C' == c) {
                            fn = _get_escseq_handler(SES_KEY_ARROW_RT);
                            if (fn) {
                                processed = fn(SES_KEY_ARROW_RT, _esc_collected);
                            }
                            goto ESC_FINISH_;
                        }
                        // 'Left Arrow' "CSI D"
                        if ('D' == c) {
                            fn = _get_escseq_handler(SES_KEY_ARROW_LF);
                            if (fn) {
                                processed = fn(SES_KEY_ARROW_LF, _esc_collected);
                            }
                            goto ESC_FINISH_;
                        }
                        // 'END' "CSI F"
                        if ('F' == c) {
                            fn = _get_escseq_handler(SES_KEY_END);
                            if (fn) {
                                processed = fn(SES_KEY_END, _esc_collected);
                            }
                            goto ESC_FINISH_;
                        }
                        // 'HOME' "CSI H"
                        if ('H' == c) {
                            fn = _get_escseq_handler(SES_KEY_HOME);
                            if (fn) {
                                processed = fn(SES_KEY_HOME, _esc_collected);
                            }
                            goto ESC_FINISH_;
                        }
                    }
                }
                // See if this is the end of other (non uppercase letter) sequences
                else if (c == '~') {
                    // It is.
                    _esc_collecting = ESC_NOT_IN_PROGRESS;
                    // See if it is one we handle
                    char ec = _esc_collected[escndx - 1]; // Get the character before the ~
                    // 'HOME' "CSI 1~"
                    if (ec == '1') {
                        fn = _get_escseq_handler(SES_KEY_HOME);
                        if (fn) {
                            processed = fn(SES_KEY_HOME, _esc_collected);
                        }
                        goto ESC_FINISH_;
                    }
                    // 'DEL-FWD' "CSI 3~"
                    if (ec == '3') {
                        fn = _get_escseq_handler(SES_KEY_FDEL);
                        if (fn) {
                            processed = fn(SES_KEY_FDEL, _esc_collected);
                        }
                        goto ESC_FINISH_;
                    }
                    // 'END' "CSI 4~"
                    if (ec == '4') {
                        fn = _get_escseq_handler(SES_KEY_END);
                        if (fn) {
                            processed = fn(SES_KEY_END, _esc_collected);
                        }
                        goto ESC_FINISH_;
                    }
                }
ESC_FINISH_:
                // Check to see if we have abandoned the sequence (not one we handle, or too many characters)
                if (!processed && (_esc_collecting == ESC_NOT_IN_PROGRESS || _esc_collecting >= ESC_CHARS_MAX)) {
                    // We have abandoned it - put the collected characters into the line buffer
                    int cti = escndx + 1;
                    if ((_line_len + cti) >= SHELL_GETLINE_MAX_LEN) {
                        cti = SHELL_GETLINE_MAX_LEN - (_line_len + 1);
                        shell_putc(BEL);
                    }
                    if (_cursor_ndx < _line_len) {
                        memmove(&_line_buf[_cursor_ndx + cti], &_line_buf[_cursor_ndx], _line_len - _cursor_ndx);
                    }
                    memmove(&_line_buf[_cursor_ndx], &_esc_collected[0], cti);
                    _line_len += cti;
                    _refresh_linert_from_cursor(false);
                    _cursor_ndx += cti;
                    term_cursor_right(cti);
                    processed = true;
                    goto FINALLY_;
                }
                else {
                    // We collected the character, so say that it is processed
                    processed = true;
                }
            }
        }
        else {
            switch (c) {
            case '\n':
            case '\r':
                // EOL - Terminate the input line and give to callback.
                _line_buf[_line_len] = '\0';
                _getline_callback = NULL;
                _input_available_handler = NULL; // Cleared when called
                handle_line(_line_buf);
                _line_len = 0;
                _cursor_ndx = 0;
                _line_buf[_line_len] = '\0';
                return (true);
                break;
            case ESC:
                // First, see if there is a handler registered for ESC. If so, let it handle it.
                processed = shell_handle_control_character(c);
                if (!processed) {
                    // Escape sequence. Most begin with CSI (ESC[)
                    _esc_collecting = 0; // mark that we need the first char of the sequence
                    _esc_collected[0] = '\0';
                    processed = true;;
                }
                break;
            }
        }
    }
PROC_AS_PLAIN_:
    if (!processed && c >= ' ' && c < DEL) {
        if (_line_len < (SHELL_GETLINE_MAX_LEN - 1)) {
            _lb_add_at_cursor(c);
        }
        else {
            // Alert them that they are at the end
            putchar(BEL);
        }
        processed = true;
    }
FINALLY_:
    return (processed);
}

void shell_build(void) {
    term_color_default();
    term_text_normal();
}

term_color_pair_t shell_color_get() {
    term_color_pair_t tc = { _color_term_text_current_fg, _color_term_text_current_bg };
    return (tc);
}

void shell_color_refresh() {
    term_color_bg(_color_term_text_current_bg);
    term_color_fg(_color_term_text_current_fg);
}

void shell_color_set(term_color_t fg, term_color_t bg) {
    _color_term_text_current_bg = bg;
    _color_term_text_current_fg = fg;
    term_color_bg(bg);
    term_color_fg(fg);
}

/**
 * @brief Must be called when an input character is ready to be processed.
 * @ingroup shell
 *
 * @param msg Nothing important in the message.
 */
void shell_do_input_char_ready() {
    if (NULL != _input_available_handler) {
        // If there is an input character, a host must be connected
        _host_connected = true;
        _input_available_handler();
    }
}

/**
 * @brief Call to Init/ReInit the terminal (connected)
 * @ingroup shell
 *
 */
void shell_do_term_init() {
    _host_welcome();
}

void shell_getline(shell_getline_callback_fn getline_cb) {
    _getline_callback = getline_cb;
    shell_register_input_available_handler(_getline_continue);
    // Use the 'continue' function to process
    _getline_continue();
}

void shell_getline_append(const char* appndstr) {
    char c;
    while ((c = *appndstr++) && (_line_len < (SHELL_GETLINE_MAX_LEN - 1))) {
        _process_char(c, false);
    }
}

void shell_getline_end() {
    int n = _line_len - _cursor_ndx;
    if (n > 0) {
        _cursor_ndx = _line_len;
        term_cursor_right(n);
    }
}

void shell_getline_home() {
    if (_cursor_ndx) {
        term_cursor_left(_cursor_ndx);
        _cursor_ndx = 0;
    }
}

const char* shell_getline_buf() {
    return (_line_buf);
}

void shell_getline_replace(const char* rplcstr) {
    int len = strlen(rplcstr);
    if (len < SHELL_GETLINE_MAX_LEN) {
        strcpy(_line_buf, rplcstr);
        _line_len = len;
        _cursor_ndx = len;
        term_cursor_on(false);
        term_cursor_bol();
        term_erase_line();
        shell_putc(CMD_PROMPT);
        shell_puts(rplcstr);
        term_cursor_on(true);
    }
}

void shell_getline_cancel(shell_input_available_handler input_handler) {
    _getline_callback = NULL;
    shell_register_input_available_handler(input_handler);
    _line_len = 0;
    _cursor_ndx = 0;
    _line_buf[_line_len] = '\0';
}

bool shell_handle_control_character(char c) {
    if (iscntrl(c)) {
        shell_control_char_handler handler_fn = _get_control_char_handler(c);
        if (handler_fn) {
            handler_fn(c);
            return (true);
        }
    }
    return (false);
}

static void _printc_for_printf_term(char c, void* arg) {
    putchar(c);
}

int shell_printf(const char* format, ...) {
    int pl = 0;
    if (_host_connected) {
        // if (_wraptext_on) {
        //     putchar('\n');
        //     pl = 1;
        // }
        va_list xArgs;
        va_start(xArgs, format);
        pl += vfctprintf(_printc_for_printf_term, NULL, format, xArgs);
        va_end(xArgs);
    }
    return (pl);
}

int shell_printferr(const char* format, ...) {
    int pl = 0;
    if (_host_connected) {
        term_color_pair_t cs = shell_color_get();
        shell_color_set(TERM_CHR_COLOR_BR_RED, TERM_CHR_COLOR_BLACK);
        // if (_wraptext_on) {
        //     putchar('\n');
        //     pl = 1;
        // }
        va_list xArgs;
        va_start(xArgs, format);
        pl += vfctprintf(_printc_for_printf_term, NULL, format, xArgs);
        va_end(xArgs);
        shell_color_set(cs.fg, cs.bg);
    }
    return (pl);
}

static void _putchar_for_app(char c) {
    if (_host_connected) {
        if ('\n' == c) {
            putchar(c);
            _wraptext_column = 0;
            return;
        }
        if (_wraptext_column == shell_COLUMNS) {
            // Printing this will cause a wrap.
            if (' ' == c) {
                // It's a space. Just print a newline instead.
                putchar('\n');
                _wraptext_column = 0;
                return;
            }
            else {
                // See if we can move back to a space
                int i = 0;
                for (; i < _wraptext_column; i++) {
                    if (' ' == _wraptext_line[_wraptext_column - i]) {
                        break;
                    }
                }
                if (i < _wraptext_column) {
                    // Yes there was a space in the line. Backup, print a '\n', then reprint to the end of line.
                    term_cursor_left(i-1);
                    term_erase_eol();
                    putchar('\n');
                    int nc = 0;
                    for (int j = ((_wraptext_column - i) + 1); j < _wraptext_column; j++) {
                        putchar(_wraptext_line[j]);
                        nc++;
                    }
                    _wraptext_column = nc;
                }
                else {
                    // No spaces in the current line. Just print a '\n' (breaking the word).
                    putchar('\n');
                    _wraptext_column = 0;
                }
            }
        }
        _wraptext_line[_wraptext_column] = c;
        putchar(c);
        _wraptext_column++;
        if ('=' == c) {
            putchar('\n');
            _wraptext_column = 0;
        }
    }
}

void shell_put_apptext(char* str) {
    // If the Command Shell is active, don't display output.
    if (CMD_SNOOZING == cmd_get_state()) {
        if (!_wraptext_on) {
            _putchar_for_app('\n');
            _wraptext_on = true;
        }
        char c;
        while ('\0' != (c = *str++)) {
            _putchar_for_app(c);
        }
    }
}

void shell_putc(uint8_t c) {
    if (_host_connected) {
        putchar(c);
    }
}

void shell_puts(const char* str) {
    if (_host_connected) {
        if (_wraptext_on) {
            putchar('\n');
            _wraptext_on = false;
        }
        int len = (int)strlen(str);
        stdio_put_string(str, len, false, true);
        stdio_flush();
    }
}

shell_control_char_handler shell_register_control_char_handler(char c, shell_control_char_handler handler_fn) {
    shell_control_char_handler ph = NULL;
    if (iscntrl(c)) {
        int ndx = (int)c;
        ph = _control_char_handler[ndx];
        _control_char_handler[ndx] = handler_fn;
    }
    return ph;
}

shell_escape_seq_handler shell_register_esc_seq_handler(sescseq_t escseq, shell_escape_seq_handler handler_fn) {
    shell_escape_seq_handler ph = _escseq_handler[escseq];
    _escseq_handler[escseq] = handler_fn;
    return ph;
}

shell_input_available_handler shell_register_input_available_handler(shell_input_available_handler handler_fn) {
    shell_input_available_handler ph = _input_available_handler;
    _input_available_handler = handler_fn;
    return ph;
}

void shell_use_output_color() {
    shell_color_set(shell_APP_COLOR_FG, shell_APP_COLOR_BG);
}

void shell_use_cmd_color() {
    shell_color_set(shell_CMD_COLOR_FG, shell_CMD_COLOR_BG);
}



void shell_start() {
    if (_started) {
        panic("!!! Shell must be stopped to be started. !!!");
    }
    _started = true;

    // Register our default Control Character and Escape Sequence handlers
    shell_register_control_char_handler(BS, _handle_backspace);
    shell_register_control_char_handler(DEL, _handle_delatcur);
    shell_register_control_char_handler('\030', _handle_cancel_line);   // ^X CAN
    shell_register_esc_seq_handler(SES_KEY_END, _ehandle_cur_end);
    shell_register_esc_seq_handler(SES_KEY_HOME, _ehandle_cur_home);
    shell_register_esc_seq_handler(SES_KEY_ARROW_LF, _ehandle_cur_left);
    shell_register_esc_seq_handler(SES_KEY_ARROW_RT, _ehandle_cur_right);
    shell_register_esc_seq_handler(SES_KEY_FDEL, _ehandle_delatcur);

    // Register our input handler with term
    term_register_notify_on_input(_input_ready_hook);
    // Do first init of the terminal. Will do another when we receive the first character.
    term_init1();
    term_text_normal();
    // Initialize the CMD module
    cmd_modinit(_handle_term_init);

    // Activate the command processor
    cmd_activate(true);
}

void shell_modinit(const char* shell_title, shell_notify_fn notify_of_char_rdy) {
    if (_modinit_called) {
        panic("!!! shell_modinit already called. !!!");
    }
    _modinit_called = true;

    _shell_title = shell_title;
    _notify_of_char_rdy = (notify_of_char_rdy ? notify_of_char_rdy : shell_do_input_char_ready);

    // Clear out the Control Char handlers
    memset(_control_char_handler, 0, sizeof(_control_char_handler));
    // Load our default control char handlers


    _esc_collecting = ESC_NOT_IN_PROGRESS;
    // Base terminal initialization
    term_init0();
}