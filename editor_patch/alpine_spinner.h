#pragma once

#include <windows.h>

// Shared spinner behaviour for the Alpine property dialogs. Initialising a field here gives it the
// stock click-step (RED dialog 209 semantics: the spinner refuses its own position change and only
// edits its buddy) plus drag scrubbing, the way RED's own spinners behave (FUN_0044B8F0).
//
// The buddy edit is written with SetWindowText, so every value change reaches the dialog as an
// EN_CHANGE exactly as typing would - a dialog that stages values for a viewport preview picks up
// clicks, drags and keystrokes through the one code path.

void alpine_spinner_init(HWND hdlg, int idc_edit, int idc_spin, float step, float min_v,
                         float max_v, int decimals);

// decimals = 0 path: the buddy holds a plain integer.
void alpine_spinner_init_int(HWND hdlg, int idc_edit, int idc_spin, int step, int min_v, int max_v);

// Applies the click-step for any spinner registered through alpine_spinner_init. Returns true when
// the notification was consumed, in which case the dialog proc must return TRUE.
bool alpine_spinner_handle_notify(HWND hdlg, LPARAM lp);
