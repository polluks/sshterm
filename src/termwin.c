/*
 * SSHTerm - SSH2 shell client
 *
 * Copyright (C) 2019-2020 Fredrik Wikstrom <fredrik@a500.org>
 *
 * This program/include file is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program/include file is distributed in the hope that it will be
 * useful, but WITHOUT ANY WARRANTY; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program (in the main directory of the SSHTerm
 * distribution in the file COPYING); if not, write to the Free Software
 * Foundation,Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "sshterm.h"
#include "menus.h"
#include "term-gc.h"

#include <intuition/menuclass.h>
#include <diskfont/diskfonttag.h>
#include <classes/window.h>
#include <gadgets/layout.h>

#include <shl-ring.h>

#include "SSHTerm_rev.h"

struct TermWindow {
	struct Screen         *Screen;
	APTR                   VisualInfo;
	APTR                   MenuStrip;
	struct MsgPort        *AppPort;
	Object                *Window;
	Object                *Layout;
	Object                *Term;
	struct Hook            IDCMPHook;
	struct Hook            OutputHook;
	struct Hook            ResizeHook;
	struct shl_ring        RingBuffer;
	UWORD                  Columns;
	UWORD                  Rows;
	BOOL                   NewSize:1;
};

enum {
	MID_INVALID,
	MID_PROJECT_MENU,
	MID_PROJECT_ICONIFY,
	MID_PROJECT_ABOUT,
	MID_PROJECT_CLOSE,
	MID_EDIT_MENU,
	MID_EDIT_COPY,
	MID_EDIT_PASTE
};

static inline ULONG GET(Object *obj, ULONG attr)
{
	ULONG result = 0;

	IIntuition->GetAttr(attr, obj, &result);

	return result;
}

static inline ULONG DGM(Object *obj, Object *winobj, Msg msg)
{
	struct Window *window;

	window = (struct Window *)GET(winobj, WINDOW_Window);

	return IIntuition->DoGadgetMethodA((struct Gadget *)obj, window, NULL, msg);
}

static ULONG term_idcmp_cb(struct Hook *hook, Object *winobj, struct IntuiMessage *imsg);
static ULONG term_output_cb(struct Hook *hook, Object *obj, struct TermOutputHookMsg *tohm);
static ULONG term_resize_cb(struct Hook *hook, Object *obj, struct TermResizeHookMsg *trhm);

static const struct NewMenu newmenus[] =
{
	{ NM_TITLE, "Project", NULL, 0, 0, (APTR)MID_PROJECT_MENU },
	{ NM_ITEM, "Iconify", "I", 0, 0, (APTR)MID_PROJECT_ICONIFY },
	{ NM_ITEM, "About...", "?", 0, 0, (APTR)MID_PROJECT_ABOUT },
	{ NM_ITEM, NM_BARLABEL, NULL, 0, 0, NULL },
	{ NM_ITEM, "Close", "K", 0, 0, (APTR)MID_PROJECT_CLOSE },
	{ NM_TITLE, "Edit", NULL, 0, 0, (APTR)MID_EDIT_MENU },
	{ NM_ITEM, "Copy", "C", 0, 0, (APTR)MID_EDIT_COPY },
	{ NM_ITEM, "Paste", "V", 0, 0, (APTR)MID_EDIT_PASTE },
	{ NM_END, NULL, NULL, 0, 0, NULL }
};

struct TermWindow *termwin_open(struct Screen *screen, ULONG max_sb)
{
	struct TermWindow *tw;
	Object *scroller;

	if (screen == NULL)
		return NULL;

	tw = malloc(sizeof(*tw));
	if (tw == NULL)
		return NULL;

	memset(tw, 0, sizeof(*tw));

	tw->Screen = screen;

	if (MenuClass == NULL)
	{
		tw->VisualInfo = IGadTools->GetVisualInfoA(tw->Screen, NULL);
	}

	tw->MenuStrip = create_menu(newmenus, tw->VisualInfo,
		NM_Menu, "Project", MA_ID, MID_PROJECT_MENU,
		NM_Item, "Iconify", MA_ID, MID_PROJECT_ICONIFY, MA_Key, "I",
		NM_Item, "About...", MA_ID, MID_PROJECT_ABOUT, MA_Key, "?",
		NM_Item, ML_SEPARATOR,
		NM_Item, "Close", MA_ID, MID_PROJECT_CLOSE, MA_Key, "K",
		NM_Menu, "Edit", MA_ID, MID_EDIT_MENU,
		NM_Item, "Copy", MA_ID, MID_EDIT_COPY, MA_Key, "C",
		NM_Item, "Paste", MA_ID, MID_EDIT_PASTE, MA_Key, "V",
		TAG_END);
	if (tw->MenuStrip == NULL)
	{
		termwin_close(tw);
		return NULL;
	}

	tw->AppPort = IExec->AllocSysObject(ASOT_PORT, NULL);
	if (tw->AppPort == NULL)
	{
		termwin_close(tw);
		return NULL;
	}

	memset(&tw->OutputHook, 0, sizeof(tw->OutputHook));
	tw->OutputHook.h_Entry = (HOOKFUNC)term_output_cb;
	tw->OutputHook.h_Data  = tw;

	memset(&tw->ResizeHook, 0, sizeof(tw->ResizeHook));
	tw->ResizeHook.h_Entry = (HOOKFUNC)term_resize_cb;
	tw->ResizeHook.h_Data  = tw;

	tw->Term = IIntuition->NewObject(TermClass, NULL,
		TERM_OutputHook, &tw->OutputHook,
		TERM_ResizeHook, &tw->ResizeHook,
		TAG_END);

	tw->Layout = IIntuition->NewObject(LayoutClass, NULL,
		/* LAYOUT_DeferLayout, TRUE, */
		LAYOUT_SpaceOuter,  FALSE,
		LAYOUT_AddChild,    tw->Term,
		TAG_END);

	memset(&tw->IDCMPHook, 0, sizeof(tw->IDCMPHook));
	tw->IDCMPHook.h_Entry = (HOOKFUNC)term_idcmp_cb;
	tw->IDCMPHook.h_Data  = tw;

	tw->Window = IIntuition->NewObject(WindowClass, NULL,
		WA_PubScreen,         tw->Screen,
		WA_Title,             VERS,
		WA_Flags,             WFLG_ACTIVATE | WFLG_CLOSEGADGET | WFLG_DRAGBAR | WFLG_DEPTHGADGET |
		                      WFLG_SIZEGADGET | WFLG_NEWLOOKMENUS | WFLG_NOCAREREFRESH,
		WA_IDCMP,             IDCMP_CLOSEWINDOW | IDCMP_MENUPICK | IDCMP_RAWKEY | IDCMP_MOUSEMOVE |
		                      IDCMP_MOUSEBUTTONS | IDCMP_EXTENDEDMOUSE,
		WINDOW_Position,      WPOS_CENTERSCREEN,
		WINDOW_BuiltInScroll, TRUE,
		WINDOW_VertProp,      TRUE,
		WINDOW_AppPort,       tw->AppPort,
		WINDOW_Icon,          AppIcon,
		WINDOW_IconNoDispose, TRUE,
		WINDOW_IconTitle,     "SSHTerm",
		WINDOW_IconifyGadget, TRUE,
		WINDOW_MenuStrip,     tw->MenuStrip,
		WINDOW_Layout,        tw->Layout,
		WINDOW_IDCMPHook,     &tw->IDCMPHook,
		WINDOW_IDCMPHookBits, IDCMP_MOUSEMOVE | IDCMP_MOUSEBUTTONS,
		TAG_END);

	if (tw->Window == NULL)
	{
		termwin_close(tw);
		return NULL;
	}

	scroller = (Object *)GET(tw->Window, WINDOW_VertObject);
	IIntuition->SetAttrs(tw->Term,
		TERM_MaxScrollback, max_sb,
		TERM_Scroller,      scroller,
		TAG_END);

	tw->Columns = (UWORD)GET(tw->Term, TERM_Columns);
	tw->Rows    = (UWORD)GET(tw->Term, TERM_Rows);

	if ((struct Window *)IIntuition->IDoMethod(tw->Window, WM_OPEN, NULL) == NULL)
	{
		termwin_close(tw);
		return NULL;
	}

	return tw;
}

void termwin_close(struct TermWindow *tw)
{
	if (tw != NULL)
	{
		if (tw->Window != NULL)
		{
			IIntuition->DisposeObject(tw->Window);
			tw->Window = NULL;
			tw->Layout = NULL;
			tw->Term   = NULL;
		}

		if (tw->AppPort != NULL)
		{
			IExec->FreeSysObject(ASOT_PORT, tw->AppPort);
			tw->AppPort = NULL;
		}

		if (tw->MenuStrip != NULL)
		{
			delete_menu(tw->MenuStrip);
			tw->MenuStrip = NULL;
		}

		if (tw->VisualInfo != NULL)
		{
			IGadTools->FreeVisualInfo(tw->VisualInfo);
			tw->VisualInfo = NULL;
		}

		free(tw);
	}
}

void termwin_set_max_sb(struct TermWindow *tw, ULONG max_sb)
{
	struct Window *window;

	window = (struct Window *)GET(tw->Window, WINDOW_Window);

	IIntuition->SetGadgetAttrs((struct Gadget *)tw->Term, window, NULL,
		TERM_MaxScrollback, max_sb,
		TAG_END);
}

void termwin_write(struct TermWindow *tw, const char *buffer, size_t len)
{
	struct tpInput tpi;

	tpi.MethodID   = TM_INPUT;
	tpi.tpi_GInfo  = NULL;
	tpi.tpi_Data   = buffer;
	tpi.tpi_Length = len;

	DGM(tw->Term, tw->Window, (Msg)&tpi);
}

ULONG termwin_get_signals(struct TermWindow *tw)
{
	return GET(tw->Window, WINDOW_SigMask);
}

static ULONG term_idcmp_cb(struct Hook *hook, Object *winobj, struct IntuiMessage *imsg)
{
	struct TermWindow *tw = hook->h_Data;
	struct tpMouse tpm;

	tpm.MethodID   = TM_HANDLEMOUSE;
	tpm.tpm_GInfo  = NULL;
	tpm.tpm_MouseX = imsg->MouseX;
	tpm.tpm_MouseY = imsg->MouseY;

	tpm.tpm_Time.Seconds      = imsg->Seconds;
	tpm.tpm_Time.Microseconds = imsg->Micros;

	if (imsg->Class == IDCMP_MOUSEBUTTONS)
	{
		tpm.tpm_Button = imsg->Code;

		if (tpm.tpm_Button == SELECTDOWN)
			IIntuition->SetAttrs(winobj, WA_ReportMouse, TRUE, TAG_END);
		else if (tpm.tpm_Button == SELECTUP)
			IIntuition->SetAttrs(winobj, WA_ReportMouse, FALSE, TAG_END);
	}
	else
	{
		tpm.tpm_Button = 0;
	}

	DGM(tw->Term, tw->Window, (Msg)&tpm);

	return 0;
}

static BOOL termwin_iconify(struct TermWindow *tw)
{
	BOOL result = FALSE;

	if (IIntuition->IDoMethod(tw->Window, WM_ICONIFY, NULL))
	{
		result = TRUE;
	}

	return result;
}

static BOOL termwin_uniconify(struct TermWindow *tw)
{
	struct Window *window;
	BOOL result = FALSE;

	window = (struct Window *)IIntuition->IDoMethod(tw->Window, WM_OPEN, NULL);
	if (window != NULL)
	{
		result = TRUE;
	}

	return result;
}

BOOL termwin_handle_input(struct TermWindow *tw)
{
	ULONG result;
	UWORD code;
	struct MenuInputData mstate;
	ULONG mid;
	struct InputEvent *ie;
	struct tpKeyboard tpk;
	struct tpGeneric tpg;
	BOOL done = FALSE;

	while ((result = IIntuition->IDoMethod(tw->Window, WM_HANDLEINPUT, &code)) != WMHI_LASTMSG)
	{
		switch (result & WMHI_CLASSMASK)
		{
			case WMHI_CLOSEWINDOW:
				done = TRUE;
				break;

			case WMHI_ICONIFY:
				termwin_iconify(tw);
				break;

			case WMHI_UNICONIFY:
				termwin_uniconify(tw);
				break;

			case WMHI_MENUPICK:
				start_menu_input(tw->MenuStrip, &mstate, code);
				while ((mid = handle_menu_input(&mstate)) != NO_MENU_ID)
				{
					switch (mid)
					{
						case MID_PROJECT_ICONIFY:
							termwin_iconify(tw);
							break;

						case MID_PROJECT_ABOUT:
							aboutwin_open(tw->Screen);
							break;

						case MID_PROJECT_CLOSE:
							done = TRUE;
							break;

						case MID_EDIT_COPY:
							tpg.MethodID  = TM_COPY;
							tpg.tpg_GInfo = NULL;

							IIntuition->IDoMethodA(tw->Term, (Msg)&tpg);
							break;

						case MID_EDIT_PASTE:
							tpg.MethodID  = TM_PASTE;
							tpg.tpg_GInfo = NULL;

							DGM(tw->Term, tw->Window, (Msg)&tpg);
							break;
					}
				}
				break;

			case WMHI_RAWKEY:
				ie = (struct InputEvent *)GET(tw->Window, WINDOW_InputEvent);

				tpk.MethodID   = TM_HANDLEKEYBOARD;
				tpk.tpk_GInfo  = NULL;
				tpk.tpk_IEvent = *ie;

				DGM(tw->Term, tw->Window, (Msg)&tpk);
				break;
		}
	}

	return done;
}

static ULONG term_output_cb(struct Hook *hook, Object *obj, struct TermOutputHookMsg *tohm)
{
	struct TermWindow *tw = hook->h_Data;
	CONST_STRPTR u8 = tohm->tohm_Data;
	ULONG len = tohm->tohm_Length;
	int r;

	r = shl_ring_push(&tw->RingBuffer, u8, len);
	if (r < 0)
	{
		IExec->DebugPrintF("shl_ring_push: %d\n", r);
	}

	return 0;
}

size_t termwin_poll(struct TermWindow *tw)
{
	return tw->RingBuffer.used;
}

ssize_t termwin_read(struct TermWindow *tw, char *buffer, size_t len)
{
	size_t n;

	n = shl_ring_copy(&tw->RingBuffer, buffer, len);
	if (n > 0)
	{
		shl_ring_pull(&tw->RingBuffer, n);
	}

	return n;
}

static ULONG term_resize_cb(struct Hook *hook, Object *obj, struct TermResizeHookMsg *trhm)
{
	struct TermWindow *tw = hook->h_Data;

	IExec->Forbid();
	tw->Columns = trhm->trhm_Columns;
	tw->Rows    = trhm->trhm_Rows;
	tw->NewSize = TRUE;
	IExec->Permit();

	return 0;
}

BOOL termwin_poll_new_size(struct TermWindow *tw)
{
	return tw->NewSize;
}

void termwin_get_size(struct TermWindow *tw, UWORD *columns, UWORD *rows)
{
	IExec->Forbid();
	tw->NewSize = FALSE;
	*columns = tw->Columns;
	*rows    = tw->Rows;
	IExec->Permit();
}

