#include <io.h>
#include <vector>
#include <map>
#include "replay.h"
#include "common.h"
#include "main.h"
#include "window.h"
#include "movie.h"
#include "archive.h"
#include "utils/xstring.h"
#include "taseditor/taseditor_config.h"

static const char* fm2ext[] = { "fm2", "fm3", 0};

int MetaPosX,MetaPosY;

// Used when deciding to automatically make the stop movie checkbox checked
static bool stopframeWasEditedByUser = false;

//the comments contained in the currently-displayed movie
static std::vector<std::wstring> currComments;

//the subtitles contained in the currently-displayed movie
static std::vector<std::string> currSubtitles;

extern FCEUGI *GameInfo;

extern TASEDITOR_CONFIG taseditorConfig;

//retains the state of the readonly checkbox and stopframe value
bool replayReadOnlySetting;
int replayStopFrameSetting = 0;

void RefreshThrottleFPS();

static char* GetReplayPath(HWND hwndDlg)
{
	char* fn=0;
	char szChoice[MAX_PATH];

	LONG lIndex = SendDlgItemMessage(hwndDlg, IDC_COMBO_FILENAME, CB_GETCURSEL, 0, 0);
	LONG lCount = SendDlgItemMessage(hwndDlg, IDC_COMBO_FILENAME, CB_GETCOUNT, 0, 0);

	// NOTE: lCount-1 is the "Browse..." list item
	if(lIndex != CB_ERR && lIndex != lCount-1)
	{
		LONG lStringLength = SendDlgItemMessage(hwndDlg, IDC_COMBO_FILENAME, CB_GETLBTEXTLEN, (WPARAM)lIndex, 0);
		if(lStringLength < MAX_PATH)
		{
			char szDrive[MAX_PATH]={0};
			char szDirectory[MAX_PATH]={0};
			char szFilename[MAX_PATH]={0};
			char szExt[MAX_PATH]={0};
			char szTemp[MAX_PATH]={0};

			SendDlgItemMessage(hwndDlg, IDC_COMBO_FILENAME, CB_GETLBTEXT, (WPARAM)lIndex, (LPARAM)szTemp);
			if(szTemp[0] && szTemp[1]!=':')
				sprintf(szChoice, ".\\%s", szTemp);
			else
				strcpy(szChoice, szTemp);

			SetCurrentDirectory(BaseDirectory.c_str());

			_splitpath(szChoice, szDrive, szDirectory, szFilename, szExt);
			if(szDrive[0]=='\0' && szDirectory[0]=='\0')
				fn=strdup(FCEU_MakePath(FCEUMKF_MOVIE, szChoice).c_str());		// need to make a full path
			else
				fn=strdup(szChoice);							// given a full path
		}
	}

	return fn;
}

static std::string GetRecordPath(HWND hwndDlg)
{
	std::string fn;
	char szChoice[MAX_PATH];
	char szDrive[MAX_PATH]={0};
	char szDirectory[MAX_PATH]={0};
	char szFilename[MAX_PATH]={0};
	char szExt[MAX_PATH]={0};

	GetDlgItemText(hwndDlg, IDC_EDIT_FILENAME, szChoice, sizeof(szChoice));

	_splitpath(szChoice, szDrive, szDirectory, szFilename, szExt);

	//make sure that there is an extension of fm2
	if(stricmp(szExt,".fm2")) {
		strcpy(szExt,".fm2");
		_makepath(szChoice,szDrive,szDirectory,szFilename,szExt);
	}

	if(szDrive[0]=='\0' && szDirectory[0]=='\0')
		fn=FCEU_MakePath(FCEUMKF_MOVIE, szChoice);		// need to make a full path
	else
		fn= szChoice;							// given a full path

	return fn;
}

static char* GetSavePath(HWND hwndDlg)
{
	char* fn=0;
	char szDrive[MAX_PATH]={0};
	char szDirectory[MAX_PATH]={0};
	char szFilename[MAX_PATH]={0};
	char szExt[MAX_PATH]={0};
	LONG lIndex = SendDlgItemMessage(hwndDlg, IDC_COMBO_RECORDFROM, CB_GETCURSEL, 0, 0);
	LONG lStringLength = SendDlgItemMessage(hwndDlg, IDC_COMBO_RECORDFROM, CB_GETLBTEXTLEN, (WPARAM)lIndex, 0);

	fn = (char*)malloc(lStringLength+1);  //CB_GETLBTEXTLEN doesn't include NULL terminator. 
	SendDlgItemMessage(hwndDlg, IDC_COMBO_RECORDFROM, CB_GETLBTEXT, (WPARAM)lIndex, (LPARAM)fn);

	_splitpath(fn, szDrive, szDirectory, szFilename, szExt);
	if(szDrive[0]=='\0' && szDirectory[0]=='\0')
	{
		char* newfn=strdup(FCEU_MakePath(FCEUMKF_STATE, fn).c_str());		// need to make a full path
		free(fn);
		fn=newfn;
	}

	return fn;
}

void UpdateReplayCommentsSubs(const char * fname) {

	MOVIE_INFO info;
	
	FCEUFILE *fp = FCEU_fopen(fname,0,"rb",0);
	if (!fp || !fp->stream)
	{
		delete fp;
		return;
	}
	EMUFILE *tmp = fp->stream->memwrap();
	if (tmp != fp->stream)
		delete fp->stream;
	fp->stream = tmp;
	bool scanok = FCEUI_MovieGetInfo(fp, info, true);
	delete fp;

	if(!scanok)
		return;

	currComments = info.comments;
	currSubtitles = info.subtitles;
}

void UpdateReplayDialog(HWND hwndDlg)
{
	int doClear=1;
	char *fn=GetReplayPath(hwndDlg);

	// remember the previous setting for the read-only checkbox
	replayReadOnlySetting = (SendDlgItemMessage(hwndDlg, IDC_CHECK_READONLY, BM_GETCHECK, 0, 0) == BST_CHECKED);

	EnableWindow(GetDlgItem(hwndDlg,IDC_BUTTON_METADATA),FALSE);

	if(fn)
	{
		MOVIE_INFO info;

		FCEUFILE *fp = FCEU_fopen(fn,0,"rb",0);
		bool ismovie = false;
		bool isarchive = false;
		if (fp && fp->stream)
		{
			isarchive = fp->isArchive();
			EMUFILE *tmp = fp->stream->memwrap();
			if (tmp != fp->stream)
				delete fp->stream;
			fp->stream = tmp;
			ismovie = FCEUI_MovieGetInfo(fp, info, false);
		}
		delete fp;
		if(ismovie)
		{
			char tmp[256];
			double div;

			sprintf(tmp, "%u", (unsigned)info.num_frames);
			SetWindowTextA(GetDlgItem(hwndDlg,IDC_LABEL_FRAMES), tmp);                   // frames
			SetDlgItemText(hwndDlg,IDC_EDIT_STOPFRAME,tmp);
			stopframeWasEditedByUser = false;

			EnableWindow(GetDlgItem(hwndDlg,IDC_CHECK_READONLY),TRUE);

			div = (FCEUI_GetCurrentVidSystem(0,0)) ? 50.006977968268290849 : 60.098813897440515532;				// PAL timing
			double tempCount = (info.num_frames / div) + 0.005; // +0.005s for rounding
			int num_seconds = (int)tempCount;
			int fraction = (int)((tempCount - num_seconds) * 100);
			int seconds = num_seconds % 60;
			int minutes = (num_seconds / 60) % 60;
			int hours = (num_seconds / 60 / 60) % 60;
			sprintf(tmp, "%02d:%02d:%02d.%02d", hours, minutes, seconds, fraction);
			SetWindowTextA(GetDlgItem(hwndDlg,IDC_LABEL_LENGTH), tmp);                   // length

			sprintf(tmp, "%u", (unsigned)info.rerecord_count);
			SetWindowTextA(GetDlgItem(hwndDlg,IDC_LABEL_UNDOCOUNT), tmp);                   // rerecord

			SendDlgItemMessage(hwndDlg,IDC_CHECK_READONLY,BM_SETCHECK,(replayReadOnlySetting ? BST_CHECKED : BST_UNCHECKED), 0);

			SetWindowText(GetDlgItem(hwndDlg,IDC_LABEL_RECORDEDFROM),info.poweron ? "Power-On" : (info.reset?"Soft-Reset":"Savestate"));

			if(isarchive) {
				EnableWindow(GetDlgItem(hwndDlg,IDC_CHECK_READONLY),FALSE);
				Button_SetCheck(GetDlgItem(hwndDlg,IDC_CHECK_READONLY),BST_CHECKED);
			} else 
				EnableWindow(GetDlgItem(hwndDlg,IDC_CHECK_READONLY),TRUE);

			//-----------
			//mbg 5/26/08 - getting rid of old movie formats

			//if(info.movie_version > 1)
			//{
				char emuStr[128];
				SetWindowText(GetDlgItem(hwndDlg,IDC_LABEL_ROMUSED),info.name_of_rom_used.c_str());
				SetWindowText(GetDlgItem(hwndDlg,IDC_LABEL_ROMCHECKSUM),md5_asciistr(info.md5_of_rom_used));
				char boolstring[4] = "On ";
				if (!info.pal)
					strcpy(boolstring, "Off");
				SetWindowText(GetDlgItem(hwndDlg,IDC_LABEL_PALUSED),boolstring);
				if (info.ppuflag)
					strcpy(boolstring, "On ");
				else
					strcpy(boolstring, "Off");
				SetWindowText(GetDlgItem(hwndDlg,IDC_LABEL_NEWPPUUSED),boolstring);



				if(info.emu_version_used < 20000 )
					sprintf(emuStr, "FCEU %d.%02d.%02d%s", info.emu_version_used/10000, (info.emu_version_used/100)%100, (info.emu_version_used)%100, info.emu_version_used < 9813 ? " (blip)" : "");
				else 
					sprintf(emuStr, "FCEUX %d.%02d.%02d", info.emu_version_used/10000, (info.emu_version_used/100)%100, (info.emu_version_used)%100);
				//else
				//{
				//	if(info.emu_version_used == 1)
				//		strcpy(emuStr, "Famtasia");
				//	else if(info.emu_version_used == 2)
				//		strcpy(emuStr, "Nintendulator");
				//	else if(info.emu_version_used == 3)
				//		strcpy(emuStr, "VirtuaNES");
				//	else
				//	{
				//		strcpy(emuStr, "(unknown)");
				//		char* dot = strrchr(fn,'.');
				//		if(dot)
				//		{
				//			if(!stricmp(dot,".fmv"))
				//				strcpy(emuStr, "Famtasia? (unknown version)");
				//			else if(!stricmp(dot,".nmv"))
				//				strcpy(emuStr, "Nintendulator? (unknown version)");
				//			else if(!stricmp(dot,".vmv"))
				//				strcpy(emuStr, "VirtuaNES? (unknown version)");
				//			else if(!stricmp(dot,".fcm"))
				//				strcpy(emuStr, "FCEU? (unknown version)");
				//		}
				//	}
				//}
				SetWindowText(GetDlgItem(hwndDlg,IDC_LABEL_EMULATORUSED),emuStr);
			//}
			//else
			//{
			//	SetWindowText(GetDlgItem(hwndDlg,IDC_LABEL_ROMUSED),"unknown");
			//	SetWindowText(GetDlgItem(hwndDlg,IDC_LABEL_ROMCHECKSUM),"unknown");
			//	SetWindowText(GetDlgItem(hwndDlg,IDC_LABEL_EMULATORUSED),"FCEU 0.98.10 (blip)");
			//}
			//--------------------

			SetWindowText(GetDlgItem(hwndDlg,IDC_LABEL_CURRCHECKSUM),md5_asciistr(GameInfo->MD5));

			// enable OK and metadata
			EnableWindow(GetDlgItem(hwndDlg,IDOK),TRUE);  
			EnableWindow(GetDlgItem(hwndDlg,IDC_BUTTON_METADATA),TRUE);
			currComments = info.comments;
			currSubtitles = info.subtitles;

			doClear = 0;
		}

		free(fn);
	}

	if(doClear)
	{
		SetWindowText(GetDlgItem(hwndDlg,IDC_LABEL_LENGTH),"");
		SetWindowText(GetDlgItem(hwndDlg,IDC_LABEL_FRAMES),"");
		SetWindowText(GetDlgItem(hwndDlg,IDC_LABEL_UNDOCOUNT),"");
		SetWindowText(GetDlgItem(hwndDlg,IDC_LABEL_ROMUSED),"");
		SetWindowText(GetDlgItem(hwndDlg,IDC_LABEL_ROMCHECKSUM),"");
		SetWindowText(GetDlgItem(hwndDlg,IDC_LABEL_RECORDEDFROM),"");
		SetWindowText(GetDlgItem(hwndDlg,IDC_LABEL_EMULATORUSED),"");
		SetWindowText(GetDlgItem(hwndDlg,IDC_LABEL_CURRCHECKSUM),md5_asciistr(GameInfo->MD5));
		SetDlgItemText(hwndDlg,IDC_EDIT_STOPFRAME,""); stopframeWasEditedByUser=false;
		EnableWindow(GetDlgItem(hwndDlg,IDC_CHECK_READONLY),FALSE);
		EnableWindow(GetDlgItem(hwndDlg,IDOK),FALSE);
	}
	EnableWindow(GetDlgItem(hwndDlg,IDC_CHECK_STOPMOVIE), !doClear);
	EnableWindow(GetDlgItem(hwndDlg,IDC_EDIT_STOPFRAME), !doClear && SendDlgItemMessage(hwndDlg, IDC_CHECK_STOPMOVIE, BM_GETCHECK, 0, 0) == BST_CHECKED);
}

// C:\fceu\movies\bla.fcm  +  C:\fceu\fceu\  ->  C:\fceu\movies\bla.fcm
// movies\bla.fcm  +  fceu\  ->  movies\bla.fcm

// C:\fceu\movies\bla.fcm  +  C:\fceu\  ->  movies\bla.fcm
void AbsoluteToRelative(char *const dst, const char *const dir, const char *const root)
{
	int i, igood=0;

	for(i = 0 ; ; i++)
	{
		int a = tolower(dir[i]);
		int b = tolower(root[i]);
		if(a == '/' || a == '\0' || a == '.') a = '\\';
		if(b == '/' || b == '\0' || b == '.') b = '\\';

		if(a != b)
		{
			igood = 0;
			break;
		}

		if(a == '\\')
			igood = i+1;

		if(!dir[i] || !root[i])
			break;
	}

//	if(igood)
//		sprintf(dst, ".\\%s", dir + igood);
//	else
		strcpy(dst, dir + igood);
}


INT_PTR CALLBACK ReplayMetadataDialogProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	RECT wrect;
	switch(uMsg)
	{
	case WM_INITDIALOG:
		{
			if (MetaPosX==-32000) MetaPosX=0; //Just in case
			if (MetaPosY==-32000) MetaPosY=0;
			SetWindowPos(hwndDlg,0,MetaPosX,MetaPosY,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOOWNERZORDER);

			//setup columns
			HWND hwndList = GetDlgItem(hwndDlg,IDC_LIST1);
			
			ListView_SetExtendedListViewStyleEx(hwndList,
                             LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES ,
                             LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES );

			ListView_SetUnicodeFormat(hwndList,TRUE);

			RECT listRect;
			GetClientRect(hwndList,&listRect);
			LVCOLUMN lvc;
			int colidx=0;
			lvc.mask = LVCF_TEXT | LVCF_WIDTH;
			lvc.pszText = "Key";
			lvc.cx = 100;
			ListView_InsertColumn(hwndList, colidx++, &lvc);
			lvc.mask = LVCF_TEXT | LVCF_WIDTH;
			lvc.pszText = "Value";
			lvc.cx = listRect.right - 100;
			ListView_InsertColumn(hwndList, colidx++, &lvc);

						
			//Display the Subtitles into the Metadata as well
			for(uint32 i=0;i<currSubtitles.size();i++)
			{
				std::string& subtitle = currSubtitles[i];
				size_t splitat = subtitle.find_first_of(' ');
				std::wstring key, value;
				//if we can't split it then call it an unnamed key
				if(splitat == std::string::npos)
				{
					value = mbstowcs(subtitle);
				} else
				{
					key = mbstowcs(subtitle.substr(0,splitat));
					value = mbstowcs(subtitle.substr(splitat+1));
				}

				LVITEM lvi;
				lvi.iItem = i;
				lvi.mask = LVIF_TEXT;
				lvi.iSubItem = 0;
				lvi.pszText = (LPSTR)key.c_str();
				SendMessageW(hwndList, LVM_INSERTITEMW, 0, (LPARAM)&lvi);
				
				lvi.iSubItem = 1;
				lvi.pszText = (LPSTR)value.c_str();
				SendMessageW(hwndList, LVM_SETITEMTEXTW, i, (LPARAM)&lvi);
			}
			
			//Display Subtitle Heading
			if (currSubtitles.size() > 0)	//If no subtitles, don't bother with this heading
			{
			std::wstring rHeading = mbstowcs(string("SUBTITLES"));
						
			LVITEM lvSubtitle;
				lvSubtitle.iItem = 0;
				lvSubtitle.mask = LVIF_TEXT;
				lvSubtitle.iSubItem = 0;
				lvSubtitle.pszText = (LPSTR)rHeading.c_str();
				SendMessageW(hwndList, LVM_INSERTITEMW, 0, (LPARAM)&lvSubtitle);
			}
				
			//Display the comments in the movie data
			for(uint32 i=0;i<currComments.size();i++)
			{
				std::wstring& comment = currComments[i];
				size_t splitat = comment.find_first_of(' ');
				std::wstring key, value;
				//if we can't split it then call it an unnamed key
				if(splitat == std::string::npos)
				{
					value = comment;
				} else
				{
					key = comment.substr(0,splitat);
					value = comment.substr(splitat+1);
				}

				LVITEM lvi;
				lvi.iItem = i;
				lvi.mask = LVIF_TEXT;
				lvi.iSubItem = 0;
				lvi.pszText = (LPSTR)key.c_str();
				SendMessageW(hwndList, LVM_INSERTITEMW, 0, (LPARAM)&lvi);
				
				lvi.iSubItem = 1;
				lvi.pszText = (LPSTR)value.c_str();
				SendMessageW(hwndList, LVM_SETITEMTEXTW, i, (LPARAM)&lvi);
			}
			
			
			
		}
		break;
	case WM_MOVE:
		if (!IsIconic(hwndDlg)) {
		GetWindowRect(hwndDlg,&wrect);
		MetaPosX = wrect.left;
		MetaPosY = wrect.top;

		#ifdef WIN32
		WindowBoundsCheckNoResize(MetaPosX,MetaPosY,wrect.right);
		#endif
		}
        break;

	case WM_COMMAND:
		switch(LOWORD(wParam))
		{
			case IDCANCEL:
				EndDialog(hwndDlg, 0);
				return TRUE;
		}
		break;
	}
	return FALSE;
}

extern char FileBase[];

void HandleScan(HWND hwndDlg, FCEUFILE* file, int& i)
{
	MOVIE_INFO info;

	bool scanok = FCEUI_MovieGetInfo(file, info, true);
	if(!scanok)
		return;

	//------------
	//attempt to match the movie with the rom
	//first, try matching md5
	//then try matching base name
	char md51 [256];
	char md52 [256];
	strcpy(md51, md5_asciistr(GameInfo->MD5));
	strcpy(md52, md5_asciistr(info.md5_of_rom_used));
	if(strcmp(md51, md52))
	{
		unsigned int k, count1=0, count2=0; //mbg merge 7/17/06 changed to uint
		for(k=0;k<strlen(md51);k++) count1 += md51[k]-'0';
		for(k=0;k<strlen(md52);k++) count2 += md52[k]-'0';
		if(count1 && count2)
			return;

		const char* tlen1=strstr(file->filename.c_str(), " (");
		const char* tlen2=strstr(FileBase, " (");
		int tlen3=tlen1?(int)(tlen1-file->filename.c_str()):file->filename.size();
		int tlen4=tlen2?(int)(tlen2-FileBase):strlen(FileBase);
		int len=MAX(0,MIN(tlen3,tlen4));
		if(strnicmp(file->filename.c_str(), FileBase, len))
		{
			char temp[512];
			strcpy(temp,FileBase);
			temp[len]='\0';
			if(!strstr(file->filename.c_str(), temp))
				return;
		}
	}
	//-------------
	//if we get here, then we had a match

	char relative[MAX_PATH];
	AbsoluteToRelative(relative, file->fullFilename.c_str(), BaseDirectory.c_str());
	SendDlgItemMessage(hwndDlg, IDC_COMBO_FILENAME, CB_INSERTSTRING, i++, (LPARAM)relative);
}


// Play Movie fast scan v1.2.
//
// v1.1 made the dialog responsive by spreading every header read over timer
// ticks.  This version attacks the actual scan cost instead: ordinary FM2/FM3
// files use a tiny header probe which only validates "version 3" and extracts
// romChecksum.  The normal MovieData parser is intentionally avoided here
// because it also constructs comments/subtitles and decodes savestate/saveram
// fields even though the movie list only needs the ROM match decision.
//
// The result is cached in memory by path + size + last-write time.  Reopening
// Play Movie in the same FCEUX session therefore does not reopen unchanged
// FM2/FM3 files at all.  Archive discovery is kept incremental because archive
// enumeration/decompression remains a different and potentially expensive path.
static const UINT_PTR REPLAY_SCAN_TIMER_ID = 0x366F;
static const UINT REPLAY_SCAN_TIMER_MS = 10;
static const DWORD REPLAY_SCAN_SLICE_MS = 5;
static const int REPLAY_SCAN_MAX_UNITS_PER_TICK = 2;
static const size_t REPLAY_QUICK_HEADER_BYTES = 4096;

struct ReplayQuickHeader
{
	bool validMovie;
	bool hasChecksum;
	MD5DATA checksum;

	ReplayQuickHeader()
		: validMovie(false)
		, hasChecksum(false)
	{
		memset(checksum.data, 0, MD5DATA::size);
	}
};

struct ReplayQuickHeaderCacheEntry
{
	DWORD fileSizeHigh;
	DWORD fileSizeLow;
	DWORD writeTimeHigh;
	DWORD writeTimeLow;
	ReplayQuickHeader header;

	ReplayQuickHeaderCacheEntry()
		: fileSizeHigh(0)
		, fileSizeLow(0)
		, writeTimeHigh(0)
		, writeTimeLow(0)
	{}
};

static std::map<std::string, ReplayQuickHeaderCacheEntry> g_replayQuickHeaderCache;

struct ReplayScanState
{
	std::vector<std::string> archiveFiles;
	size_t archivePos;
	size_t archiveMemberPos;
	ArchiveScanRecord archiveScan;
	std::string currentArchive;
	int items;
	bool userChangedSelection;

	ReplayScanState()
		: archivePos(0)
		, archiveMemberPos(0)
		, items(0)
		, userChangedSelection(false)
	{}
};

static ReplayScanState* GetReplayScanState(HWND hwndDlg)
{
	return (ReplayScanState*)GetWindowLongPtr(hwndDlg, GWLP_USERDATA);
}

static void DestroyReplayScanState(HWND hwndDlg)
{
	KillTimer(hwndDlg, REPLAY_SCAN_TIMER_ID);
	ReplayScanState* state = GetReplayScanState(hwndDlg);
	if (state)
	{
		SetWindowLongPtr(hwndDlg, GWLP_USERDATA, 0);
		delete state;
	}
}

static bool IsReplayPlainMovieExtension(const std::string& ext)
{
	return ext == "fm2" || ext == "fm3";
}

static bool IsReplayArchiveExtension(const std::string& ext)
{
	return ext == "zip" || ext == "rar" || ext == "7z";
}

static bool IsZeroReplayMD5(const MD5DATA& md5)
{
	for (int i = 0; i < MD5DATA::size; ++i)
		if (md5.data[i] != 0)
			return false;
	return true;
}

// FM2 generated by FCEUX writes romChecksum near the beginning of the header
// (before comments, subtitles, savestate and saveram).  A single 4 KiB read is
// enough for normal files and avoids parsing/decoding every unrelated header
// field.  If a hand-edited movie moves romChecksum outside this probe, it is
// treated like an old checksum-less movie and falls back to the legacy filename
// matching rule; selecting it still goes through the full parser as before.
static ReplayQuickHeader ReadReplayQuickHeader(const std::string& filename)
{
	ReplayQuickHeader result;
	EMUFILE_FILE* stream = FCEUD_UTF8_fstream(filename.c_str(), "rb");
	if (!stream)
		return result;

	char buffer[REPLAY_QUICK_HEADER_BYTES + 1];
	const size_t got = stream->fread(buffer, REPLAY_QUICK_HEADER_BYTES);
	delete stream;

	if (got < 9 || memcmp(buffer, "version 3", 9))
		return result;

	result.validMovie = true;
	buffer[got] = '\0';

	const char* p = buffer;
	const char* end = buffer + got;
	while (p < end)
	{
		const char* lineEnd = (const char*)memchr(p, '\n', (size_t)(end - p));
		if (!lineEnd)
			lineEnd = end;

		const char* logicalEnd = lineEnd;
		if (logicalEnd > p && logicalEnd[-1] == '\r')
			--logicalEnd;

		if (p < logicalEnd && *p == '|')
			break;

		static const char checksumKey[] = "romChecksum";
		const size_t keyLen = sizeof(checksumKey) - 1;
		if ((size_t)(logicalEnd - p) > keyLen &&
			!memcmp(p, checksumKey, keyLen) &&
			(p[keyLen] == ' ' || p[keyLen] == '\t'))
		{
			const char* value = p + keyLen;
			while (value < logicalEnd && (*value == ' ' || *value == '\t'))
				++value;

			std::string checksumText(value, logicalEnd);
			if (StringToBytes(checksumText, result.checksum.data, MD5DATA::size))
				result.hasChecksum = true;
			break;
		}

		if (lineEnd == end)
			break;
		p = lineEnd + 1;
	}

	return result;
}

static bool ReplayQuickCacheStampMatches(const ReplayQuickHeaderCacheEntry& entry, const WIN32_FIND_DATA& wfd)
{
	return entry.fileSizeHigh == wfd.nFileSizeHigh &&
		entry.fileSizeLow == wfd.nFileSizeLow &&
		entry.writeTimeHigh == wfd.ftLastWriteTime.dwHighDateTime &&
		entry.writeTimeLow == wfd.ftLastWriteTime.dwLowDateTime;
}

static const ReplayQuickHeader& GetReplayQuickHeaderCached(const std::string& filename, const WIN32_FIND_DATA& wfd)
{
	std::map<std::string, ReplayQuickHeaderCacheEntry>::iterator it = g_replayQuickHeaderCache.find(filename);
	if (it != g_replayQuickHeaderCache.end() && ReplayQuickCacheStampMatches(it->second, wfd))
		return it->second.header;

	ReplayQuickHeaderCacheEntry entry;
	entry.fileSizeHigh = wfd.nFileSizeHigh;
	entry.fileSizeLow = wfd.nFileSizeLow;
	entry.writeTimeHigh = wfd.ftLastWriteTime.dwHighDateTime;
	entry.writeTimeLow = wfd.ftLastWriteTime.dwLowDateTime;
	entry.header = ReadReplayQuickHeader(filename);

	g_replayQuickHeaderCache[filename] = entry;
	return g_replayQuickHeaderCache[filename].header;
}

static bool ReplayFilenameFallbackMatchesCurrentGame(const std::string& filename)
{
	const char* tlen1 = strstr(filename.c_str(), " (");
	const char* tlen2 = strstr(FileBase, " (");
	int tlen3 = tlen1 ? (int)(tlen1 - filename.c_str()) : (int)filename.size();
	int tlen4 = tlen2 ? (int)(tlen2 - FileBase) : (int)strlen(FileBase);
	int len = MAX(0, MIN(tlen3, tlen4));
	if (!strnicmp(filename.c_str(), FileBase, len))
		return true;

	char temp[512];
	strcpy(temp, FileBase);
	temp[len] = '\0';
	return strstr(filename.c_str(), temp) != 0;
}

static bool ReplayQuickHeaderMatchesCurrentGame(const ReplayQuickHeader& header, const std::string& filename)
{
	if (!header.validMovie)
		return false;

	if (header.hasChecksum)
	{
		if (!memcmp(header.checksum.data, GameInfo->MD5.data, MD5DATA::size))
			return true;

		// Preserve the original behavior: a real non-zero checksum mismatch is
		// a hard reject; zero/unknown checksums fall back to the filename rule.
		if (!IsZeroReplayMD5(header.checksum) && !IsZeroReplayMD5(GameInfo->MD5))
			return false;
	}

	return ReplayFilenameFallbackMatchesCurrentGame(filename);
}

static void InsertPlainReplayMatch(HWND hwndDlg, const std::string& filename, int& items)
{
	char relative[MAX_PATH];
	AbsoluteToRelative(relative, filename.c_str(), BaseDirectory.c_str());
	SendDlgItemMessage(hwndDlg, IDC_COMBO_FILENAME, CB_INSERTSTRING, items++, (LPARAM)relative);
}

// Ordinary FM2/FM3 are deliberately completed in one pass before the dialog is
// shown.  The work per file is now just: directory metadata lookup (already in
// WIN32_FIND_DATA), optional cache hit, and at most one 4 KiB read on a cache
// miss.  This is the actual search-speed optimization requested for v1.2.
static void FastScanPlainReplayMovies(HWND hwndDlg, int& items, std::vector<std::string>& archiveFiles)
{
#define NUM_OF_MOVIEGLOB_PATHS 1
	char* findGlob[NUM_OF_MOVIEGLOB_PATHS] = { strdup(FCEU_MakeFName(FCEUMKF_MOVIEGLOB, 0, 0).c_str()) };

	for (int j = 0; j < NUM_OF_MOVIEGLOB_PATHS; ++j)
	{
		char* temp = 0;
		do
		{
			temp = strchr(findGlob[j], '/');
			if (temp)
				*temp = '\\';
		} while (temp);
	}

	for (int j = 0; j < NUM_OF_MOVIEGLOB_PATHS; ++j)
	{
		char globBase[512];
		strcpy(globBase, findGlob[j]);
		globBase[strlen(globBase) - 5] = '\0';

		WIN32_FIND_DATA wfd;
		memset(&wfd, 0, sizeof(wfd));
		HANDLE hFind = FindFirstFile(findGlob[j], &wfd);
		if (hFind == INVALID_HANDLE_VALUE)
			continue;

		do
		{
			if (wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				continue;

			std::string ext = getExtension(wfd.cFileName);
			if (!IsReplayPlainMovieExtension(ext) && !IsReplayArchiveExtension(ext))
				continue;

			char filename[512];
			sprintf(filename, "%s%s", globBase, wfd.cFileName);

			if (IsReplayArchiveExtension(ext))
			{
				archiveFiles.push_back(filename);
				continue;
			}

			const ReplayQuickHeader& header = GetReplayQuickHeaderCached(filename, wfd);
			if (ReplayQuickHeaderMatchesCurrentGame(header, filename))
				InsertPlainReplayMatch(hwndDlg, filename, items);
		} while (FindNextFile(hFind, &wfd));

		FindClose(hFind);
	}

	for (int j = 0; j < NUM_OF_MOVIEGLOB_PATHS; ++j)
		free(findGlob[j]);
#undef NUM_OF_MOVIEGLOB_PATHS
}

// CB_INSERTSTRING before the final "Browse..." entry can move that entry one
// slot down. If Browse was selected, keep it selected while archives arrive.
static void PreserveBrowseSelectionAfterInsert(HWND hwndDlg, LONG oldCount, LONG oldSelection, int newMovieCount)
{
	if (oldSelection != CB_ERR && oldSelection == oldCount - 1)
		SendDlgItemMessage(hwndDlg, IDC_COMBO_FILENAME, CB_SETCURSEL, newMovieCount, 0);
}

static bool PrepareNextReplayArchive(ReplayScanState* state)
{
	while (state && state->archivePos < state->archiveFiles.size())
	{
		state->currentArchive = state->archiveFiles[state->archivePos++];
		SetCurrentDirectory(BaseDirectory.c_str());
		state->archiveScan = FCEUD_ScanArchive(state->currentArchive);
		if (!state->archiveScan.isArchive())
		{
			state->currentArchive.clear();
			continue;
		}

		state->archiveScan.files.FilterByExtension(fm2ext);
		state->archiveMemberPos = 0;
		if (!state->archiveScan.files.empty())
			return true;

		state->currentArchive.clear();
	}
	return false;
}

static void ScanOneArchiveReplay(HWND hwndDlg, ReplayScanState* state)
{
	if (!state)
		return;

	if (state->currentArchive.empty() || state->archiveMemberPos >= state->archiveScan.files.size())
	{
		state->currentArchive.clear();
		if (!PrepareNextReplayArchive(state))
			return;
	}

	LONG oldCount = SendDlgItemMessage(hwndDlg, IDC_COMBO_FILENAME, CB_GETCOUNT, 0, 0);
	LONG oldSelection = SendDlgItemMessage(hwndDlg, IDC_COMBO_FILENAME, CB_GETCURSEL, 0, 0);
	int oldItems = state->items;

	FCEUARCHIVEFILEINFO_ITEM& item = state->archiveScan.files[state->archiveMemberPos++];
	SetCurrentDirectory(BaseDirectory.c_str());
	FCEUFILE* fp = FCEUD_OpenArchiveIndex(state->archiveScan, state->currentArchive, item.index);
	if (fp)
	{
		HandleScan(hwndDlg, fp, state->items);
		delete fp;
	}

	if (state->items != oldItems)
		PreserveBrowseSelectionAfterInsert(hwndDlg, oldCount, oldSelection, state->items);

	if (state->archiveMemberPos >= state->archiveScan.files.size())
		state->currentArchive.clear();
}

static bool ReplayArchiveScanIsFinished(const ReplayScanState* state)
{
	if (!state)
		return true;
	if (!state->currentArchive.empty())
		return false;
	return state->archivePos >= state->archiveFiles.size();
}

static void FinishReplayArchiveScan(HWND hwndDlg, ReplayScanState* state)
{
	if (!state)
		return;

	KillTimer(hwndDlg, REPLAY_SCAN_TIMER_ID);
	if (!state->userChangedSelection && state->items > 0)
	{
		SendDlgItemMessage(hwndDlg, IDC_COMBO_FILENAME, CB_SETCURSEL, state->items - 1, 0);
		UpdateReplayDialog(hwndDlg);
	}

	SetWindowLongPtr(hwndDlg, GWLP_USERDATA, 0);
	delete state;
}

static void ReplayArchiveScanTick(HWND hwndDlg)
{
	ReplayScanState* state = GetReplayScanState(hwndDlg);
	if (!state)
		return;

	DWORD started = GetTickCount();
	int units = 0;
	while (units < REPLAY_SCAN_MAX_UNITS_PER_TICK && (GetTickCount() - started) < REPLAY_SCAN_SLICE_MS)
	{
		if (!state->currentArchive.empty() || state->archivePos < state->archiveFiles.size())
		{
			ScanOneArchiveReplay(hwndDlg, state);
			++units;
			continue;
		}
		break;
	}

	if (ReplayArchiveScanIsFinished(state))
		FinishReplayArchiveScan(hwndDlg, state);
}

INT_PTR CALLBACK ReplayDialogProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch(uMsg)
	{
	case WM_INITDIALOG:
		{
			SendDlgItemMessage(hwndDlg, IDC_CHECK_READONLY, BM_SETCHECK, replayReadOnlySetting?BST_CHECKED:BST_UNCHECKED, 0);
			SendDlgItemMessage(hwndDlg, IDC_CHECK_STOPMOVIE, BM_SETCHECK, BST_UNCHECKED, 0);

			int items = 0;
			std::vector<std::string> archiveFiles;
			FastScanPlainReplayMovies(hwndDlg, items, archiveFiles);

			if (items > 0)
				SendDlgItemMessage(hwndDlg, IDC_COMBO_FILENAME, CB_SETCURSEL, items - 1, 0);
			SendDlgItemMessage(hwndDlg, IDC_COMBO_FILENAME, CB_INSERTSTRING, items, (LPARAM)"Browse...");
			if (items == 0)
				SendDlgItemMessage(hwndDlg, IDC_COMBO_FILENAME, CB_SETCURSEL, 0, 0);
			UpdateReplayDialog(hwndDlg);

			// Only archives retain incremental loading. Ordinary FM2/FM3 are fully
			// discovered above using the optimized synchronous fast path.
			if (!archiveFiles.empty())
			{
				ReplayScanState* state = new ReplayScanState();
				state->archiveFiles.swap(archiveFiles);
				state->items = items;
				SetWindowLongPtr(hwndDlg, GWLP_USERDATA, (LONG_PTR)state);
				SetTimer(hwndDlg, REPLAY_SCAN_TIMER_ID, REPLAY_SCAN_TIMER_MS, 0);
			}
		}

		SetFocus(GetDlgItem(hwndDlg, IDC_COMBO_FILENAME));
		return FALSE;

	case WM_TIMER:
		if (wParam == REPLAY_SCAN_TIMER_ID)
		{
			ReplayArchiveScanTick(hwndDlg);
			return TRUE;
		}
		break;

	case WM_COMMAND:
		if (HIWORD(wParam) == EN_CHANGE)
		{
			if (LOWORD(wParam) == IDC_EDIT_STOPFRAME) // Check if Stop movie at value has changed
			{
				if (stopframeWasEditedByUser)
				{
				HWND hwnd1 = GetDlgItem(hwndDlg,IDC_CHECK_STOPMOVIE);
				Button_SetCheck(hwnd1,BST_CHECKED);
				stopframeWasEditedByUser = true;
				}
				else
					stopframeWasEditedByUser = true;
			}
		}

		if (HIWORD(wParam) == CBN_SELCHANGE)
		{
			ReplayScanState* state = GetReplayScanState(hwndDlg);
			if (state)
				state->userChangedSelection = true;
			UpdateReplayDialog(hwndDlg);
		} else if(HIWORD(wParam) == CBN_CLOSEUP)
		{
			LONG lCount = SendDlgItemMessage(hwndDlg, IDC_COMBO_FILENAME, CB_GETCOUNT, 0, 0);
			LONG lIndex = SendDlgItemMessage(hwndDlg, IDC_COMBO_FILENAME, CB_GETCURSEL, 0, 0);
			if (lIndex != CB_ERR && lIndex == lCount-1)
				SendMessage(hwndDlg, WM_COMMAND, (WPARAM)IDOK, 0);		// send an OK notification to open the file browser
		} else
		{
			int wID = LOWORD(wParam);
			switch(wID)
			{
			case IDC_CHECK_STOPMOVIE:
				EnableWindow(GetDlgItem(hwndDlg, IDC_EDIT_STOPFRAME), SendDlgItemMessage(hwndDlg, IDC_CHECK_STOPMOVIE, BM_GETCHECK, 0, 0) == BST_CHECKED);
				break;
			case IDC_BUTTON_METADATA:
				DialogBoxParam(fceu_hInstance, "IDD_REPLAY_METADATA", hwndDlg, ReplayMetadataDialogProc, (LPARAM)0);
				break;

			case IDOK:
				{
					LONG lCount = SendDlgItemMessage(hwndDlg, IDC_COMBO_FILENAME, CB_GETCOUNT, 0, 0);
					LONG lIndex = SendDlgItemMessage(hwndDlg, IDC_COMBO_FILENAME, CB_GETCURSEL, 0, 0);
					if(lIndex != CB_ERR)
					{
						if(lIndex == lCount-1)
						{
							// The common file dialog runs its own modal message loop. Pause
							// the remaining archive scan while it is open.
							ReplayScanState* state = GetReplayScanState(hwndDlg);
							if (state)
								KillTimer(hwndDlg, REPLAY_SCAN_TIMER_ID);
							// pop open a file browser...
							char *pn=strdup(FCEU_GetPath(FCEUMKF_MOVIE).c_str());
							char szFile[MAX_PATH]={0};
							OPENFILENAME ofn;
							//int nRet; //mbg merge 7/17/06 removed

							memset(&ofn, 0, sizeof(ofn));
							ofn.lStructSize = sizeof(ofn);
							ofn.hwndOwner = hwndDlg;
							ofn.lpstrFilter = "FCEUX Movie Files (*.fm2), TAS Editor Projects (*.fm3)\0*.fm2;*.fm3\0FCEUX Movie Files (*.fm2)\0*.fm2\0Archive Files (*.zip,*.rar,*.7z)\0*.zip;*.rar;*.7z\0All Files (*.*)\0*.*\0\0";
							ofn.lpstrFile = szFile;
							ofn.nMaxFile = sizeof(szFile);
							ofn.lpstrInitialDir = pn;
							ofn.Flags = OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
							ofn.lpstrDefExt = "fm2";
							ofn.lpstrTitle = "Play Movie from File";
	
							if(GetOpenFileName(&ofn))
							{
								// The user picked an explicit movie, so any remaining archive scan is
								// no longer needed.
								DestroyReplayScanState(hwndDlg);
								state = 0;
								char relative[MAX_PATH*2];
								AbsoluteToRelative(relative, szFile, BaseDirectory.c_str());
								
								//replay system requires this to stay put.
								SetCurrentDirectory(BaseDirectory.c_str());

								FCEUFILE* fp = FCEU_fopen(relative,0,"rb",0,-1,fm2ext);
								if(!fp)
									goto abort;
								strcpy(relative,fp->fullFilename.c_str());
								delete fp;

								LONG lOtherIndex = SendDlgItemMessage(hwndDlg, IDC_COMBO_FILENAME, CB_FINDSTRING, (WPARAM)-1, (LPARAM)relative);
								if(lOtherIndex != CB_ERR)
								{
									// select already existing string
									SendDlgItemMessage(hwndDlg, IDC_COMBO_FILENAME, CB_SETCURSEL, lOtherIndex, 0);
									UpdateReplayDialog(hwndDlg);
								} else
								{
									SendDlgItemMessage(hwndDlg, IDC_COMBO_FILENAME, CB_INSERTSTRING, lIndex, (LPARAM)relative);
									SendDlgItemMessage(hwndDlg, IDC_COMBO_FILENAME, CB_SETCURSEL, lIndex, 0);
									// CB_SETCURSEL does not send CBN_SELCHANGE. Refresh explicitly.
									UpdateReplayDialog(hwndDlg);
								}
								// restore focus to the dialog
								SetFocus(GetDlgItem(hwndDlg, IDC_COMBO_FILENAME));
							}
							else if (state)
							{
								// Browse was canceled: continue the archive scan from where it paused.
								SetTimer(hwndDlg, REPLAY_SCAN_TIMER_ID, REPLAY_SCAN_TIMER_MS, 0);
							}
						abort:

							free(pn);
						}
						else
						{
							extern unsigned int FrozenAddressCount;
							if(FrozenAddressCount)
							{
								char ch[512];
								sprintf(ch, "You have %d activated cheats. If this is not your intentional, it can cause playback prblems! Do you want to disable all of them and continue?", FrozenAddressCount);
								switch(MessageBox(hwndDlg, ch, "Movie playing problem", MB_YESNOCANCEL|MB_ICONEXCLAMATION))
								{
									case IDCANCEL:
										return TRUE;
									case IDYES:
										extern void DisableAllCheats();
										DisableAllCheats();
										extern void UpdateCheatRelatedWindow();
										UpdateCheatRelatedWindow();
								}

							}
							// user had made their choice
							// TODO: warn the user when they open a movie made with a different ROM
							char* fn=GetReplayPath(hwndDlg);
							//char TempArray[16]; //mbg merge 7/17/06 removed
							replayReadOnlySetting = (SendDlgItemMessage(hwndDlg, IDC_CHECK_READONLY, BM_GETCHECK, 0, 0) == BST_CHECKED);

							char offset1Str[32]={0};

							SendDlgItemMessage(hwndDlg, IDC_EDIT_STOPFRAME, WM_GETTEXT, (WPARAM)32, (LPARAM)offset1Str);
							replayStopFrameSetting = (SendDlgItemMessage(hwndDlg, IDC_CHECK_STOPMOVIE, BM_GETCHECK,0,0) == BST_CHECKED)? strtol(offset1Str,0,10):0;

							DestroyReplayScanState(hwndDlg);
							EndDialog(hwndDlg, (INT_PTR)fn);
						}
					}
				}
				return TRUE;

			case IDCANCEL:
				DestroyReplayScanState(hwndDlg);
				EndDialog(hwndDlg, 0);
				return TRUE;
			}
		}
		return FALSE;

	case WM_DESTROY:
		DestroyReplayScanState(hwndDlg);
		break;

	case WM_CTLCOLORSTATIC:
		if ((HWND)lParam == GetDlgItem(hwndDlg, IDC_LABEL_CURRCHECKSUM))
		{
			// draw the md5 sum in red if it's different from the md5 of the rom used in the replay
			HDC hdcStatic = (HDC)wParam;
			char szMd5Text[35];
			GetDlgItemText(hwndDlg, IDC_LABEL_ROMCHECKSUM, szMd5Text, 35);
			if (!strlen(szMd5Text) || !strcmp(szMd5Text, "unknown") || !strcmp(szMd5Text, "00000000000000000000000000000000") || !strcmp(szMd5Text, md5_asciistr(GameInfo->MD5)))
				SetTextColor(hdcStatic, RGB(0,0,0));		// use black color for a match (or no comparison)
			else
				SetTextColor(hdcStatic, RGB(255,0,0));		// use red for a mismatch
			SetBkMode((HDC)wParam,TRANSPARENT);
			return (INT_PTR)GetSysColorBrush(COLOR_BTNFACE);
		} else if ((HWND)lParam == GetDlgItem(hwndDlg, IDC_LABEL_NEWPPUUSED))
		{
			HDC hdcStatic = (HDC)wParam;
			char szMd5Text[35];
			GetDlgItemText(hwndDlg, IDC_LABEL_NEWPPUUSED, szMd5Text, 35);
			bool want_newppu = (strcmp(szMd5Text, "Off") != 0);
			extern int newppu;
			if ((want_newppu && newppu) || (!want_newppu && !newppu))
				SetTextColor(hdcStatic, RGB(0,0,0));		// use black color for a match
			else
				SetTextColor(hdcStatic, RGB(255,0,0));		// use red for a mismatch
			SetBkMode((HDC)wParam,TRANSPARENT);
			return (INT_PTR)GetSysColorBrush(COLOR_BTNFACE);
		} else
		{
			return FALSE;
		}
	}

	return FALSE;
};

static void UpdateRecordDialog(HWND hwndDlg)
{
	int enable=0;

	std::string fn=GetRecordPath(hwndDlg);

	if(fn!="")
	{
		if(access(fn.c_str(), F_OK) ||
			!access(fn.c_str(), W_OK))
		{
			LONG lCount = SendDlgItemMessage(hwndDlg, IDC_COMBO_RECORDFROM, CB_GETCOUNT, 0, 0);
			LONG lIndex = SendDlgItemMessage(hwndDlg, IDC_COMBO_RECORDFROM, CB_GETCURSEL, 0, 0);
			if(lIndex != lCount-1)
			{
				enable=1;
			}
		}

	}

	EnableWindow(GetDlgItem(hwndDlg,IDOK),enable ? TRUE : FALSE);
}

static void UpdateRecordDialogPath(HWND hwndDlg, const std::string &fname)
{
	const std::string &baseMovieDir = FCEU_GetPath(FCEUMKF_MOVIE);
	char* fn=0;

	// display a shortened filename if the file exists in the base movie directory
	if(!strncmp(fname.c_str(), baseMovieDir.c_str(), baseMovieDir.size()))
	{
		char szDrive[MAX_PATH]={0};
		char szDirectory[MAX_PATH]={0};
		char szFilename[MAX_PATH]={0};
		char szExt[MAX_PATH]={0};

		_splitpath(fname.c_str(), szDrive, szDirectory, szFilename, szExt);
		fn=(char*)malloc(strlen(szFilename)+strlen(szExt)+1);
		_makepath(fn, "", "", szFilename, szExt);
	}
	else
		fn=strdup(fname.c_str());

	if(fn)
	{
		SetWindowText(GetDlgItem(hwndDlg,IDC_EDIT_FILENAME),fn);				// FIXME:  make utf-8?
		free(fn);
	}
}

static INT_PTR CALLBACK RecordDialogProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	static struct CreateMovieParameters* p = NULL;

	switch(uMsg)
	{
	case WM_INITDIALOG:
		p = (struct CreateMovieParameters*)lParam;
		UpdateRecordDialogPath(hwndDlg, p->szFilename);
		p->szFilename = "";

		if (strlen(taseditorConfig.lastAuthorName))
		{
			// convert UTF8 char* string to Unicode wstring
			wchar_t savedAuthorName[AUTHOR_NAME_MAX_LEN] = {0};
			MultiByteToWideChar(CP_UTF8, 0, taseditorConfig.lastAuthorName, -1, savedAuthorName, AUTHOR_NAME_MAX_LEN);
			p->authorName = savedAuthorName;
		} else
		{
			p->authorName = L"";
		}
		SendMessage(GetDlgItem(hwndDlg, IDC_EDIT_AUTHOR), CCM_SETUNICODEFORMAT, TRUE, 0);
		SetDlgItemTextW(hwndDlg, IDC_EDIT_AUTHOR, (LPCWSTR)(p->authorName.c_str()));

		// Populate the "record from..." dialog
		{
			char* findGlob=strdup(FCEU_MakeFName(FCEUMKF_STATEGLOB, 0, 0).c_str());
			WIN32_FIND_DATA wfd;
			HANDLE hFind;
			int i=0;

			SendDlgItemMessage(hwndDlg, IDC_COMBO_RECORDFROM, CB_INSERTSTRING, i++, (LPARAM)"Start");
			SendDlgItemMessage(hwndDlg, IDC_COMBO_RECORDFROM, CB_INSERTSTRING, i++, (LPARAM)"Now");
			SendDlgItemMessage(hwndDlg, IDC_COMBO_RECORDFROM, CB_INSERTSTRING, i++, (LPARAM)"SaveRam");

			memset(&wfd, 0, sizeof(wfd));
			hFind = FindFirstFile(findGlob, &wfd);
			if(hFind != INVALID_HANDLE_VALUE)
			{
				do
				{
					if ((wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
						(wfd.dwFileAttributes & FILE_ATTRIBUTE_READONLY))
						continue;

					if (strlen(wfd.cFileName) < 4 ||
						!strcmp(wfd.cFileName + (strlen(wfd.cFileName) - 4), ".fm2"))
						continue;

					SendDlgItemMessage(hwndDlg, IDC_COMBO_RECORDFROM, CB_INSERTSTRING, i++, (LPARAM)wfd.cFileName);
				} while(FindNextFile(hFind, &wfd));
				FindClose(hFind);
			}
			free(findGlob);

			SendDlgItemMessage(hwndDlg, IDC_COMBO_RECORDFROM, CB_INSERTSTRING, i++, (LPARAM)"Browse...");
			SendDlgItemMessage(hwndDlg, IDC_COMBO_RECORDFROM, CB_SETCURSEL, p->recordFrom, 0);
		}
		UpdateRecordDialog(hwndDlg);

		return TRUE;

	case WM_COMMAND:
		if(HIWORD(wParam) == CBN_SELCHANGE)
		{
			LONG lIndex = SendDlgItemMessage(hwndDlg, IDC_COMBO_RECORDFROM, CB_GETCURSEL, 0, 0);
			if(lIndex == CB_ERR)
			{
				// fix listbox selection
				SendDlgItemMessage(hwndDlg, IDC_COMBO_RECORDFROM, CB_SETCURSEL, (WPARAM)0, 0);
			}
			UpdateRecordDialog(hwndDlg);
			return TRUE;
		}
		else if(HIWORD(wParam) == CBN_CLOSEUP)
		{
			LONG lCount = SendDlgItemMessage(hwndDlg, IDC_COMBO_RECORDFROM, CB_GETCOUNT, 0, 0);
			LONG lIndex = SendDlgItemMessage(hwndDlg, IDC_COMBO_RECORDFROM, CB_GETCURSEL, 0, 0);
			if (lIndex != CB_ERR && lIndex == lCount-1)
			{
				OPENFILENAME ofn;
				char szChoice[MAX_PATH]={0};

				// pop open a file browser to choose the savestate
				memset(&ofn, 0, sizeof(ofn));
				ofn.lStructSize = sizeof(ofn);
				ofn.hwndOwner = hwndDlg;
				ofn.lpstrFilter = "FCEU Save State (*.fc?)\0*.fc?\0\0";
				ofn.lpstrFile = szChoice;
				ofn.lpstrDefExt = "fcs";
				ofn.nMaxFile = MAX_PATH;
				if(GetOpenFileName(&ofn))
				{
					SendDlgItemMessage(hwndDlg, IDC_COMBO_RECORDFROM, CB_INSERTSTRING, lIndex, (LPARAM)szChoice);
					SendDlgItemMessage(hwndDlg, IDC_COMBO_RECORDFROM, CB_SETCURSEL, (WPARAM)lIndex, 0);
				}
				else
					UpdateRecordDialog(hwndDlg);
			}
			return TRUE;
		}
		else if(HIWORD(wParam) == EN_CHANGE && LOWORD(wParam) == IDC_EDIT_FILENAME)
		{
			UpdateRecordDialog(hwndDlg);
		}
		else
		{
			switch(LOWORD(wParam))
			{
			case IDOK:
				{
					extern unsigned int FrozenAddressCount;
					if (FrozenAddressCount)
					{
						char ch[512];
						sprintf(ch, "You have %d activated cheats. If this is not your intentional, it can cause playback prblems! Do you want to disable all of them and continue?", FrozenAddressCount);
						switch (MessageBox(hwndDlg, ch, "Movie recording problem", MB_YESNOCANCEL | MB_ICONEXCLAMATION))
						{
							case IDCANCEL:
								return TRUE;
							case IDYES:
								extern void DisableAllCheats();
								DisableAllCheats();
								extern void UpdateCheatRelatedWindow();
								UpdateCheatRelatedWindow();
						}
					}

					LONG lIndex = SendDlgItemMessage(hwndDlg, IDC_COMBO_RECORDFROM, CB_GETCURSEL, 0, 0);
					p->szFilename = GetRecordPath(hwndDlg);
					p->recordFrom = (int)lIndex;
					// save author name in params and in config (converted to multibyte char*)
					wchar_t authorName[AUTHOR_NAME_MAX_LEN] = {0};
					GetDlgItemTextW(hwndDlg, IDC_EDIT_AUTHOR, (LPWSTR)authorName, AUTHOR_NAME_MAX_LEN);
					p->authorName = authorName;
					if (p->authorName == L"")
						taseditorConfig.lastAuthorName[0] = 0;
					else
						// convert Unicode wstring to UTF8 char* string
						WideCharToMultiByte(CP_UTF8, 0, (p->authorName).c_str(), -1, taseditorConfig.lastAuthorName, AUTHOR_NAME_MAX_LEN, 0, 0);

					if (lIndex >= 3)
						p->szSavestateFilename = GetSavePath(hwndDlg);
					EndDialog(hwndDlg, 1);
				}
				return TRUE;

			case IDCANCEL:
				EndDialog(hwndDlg, 0);
				return TRUE;

			case IDC_BUTTON_BROWSEFILE:
				{
					OPENFILENAME ofn;
					char szChoice[MAX_PATH]={0};

					// browse button
					memset(&ofn, 0, sizeof(ofn));
					ofn.lStructSize = sizeof(ofn);
					ofn.hwndOwner = hwndDlg;
					ofn.lpstrFilter = "FCEUX Movie File (*.fm2)\0*.fm2\0All Files (*.*)\0*.*\0\0";
					ofn.lpstrFile = szChoice;
					ofn.lpstrDefExt = "fm2";
					ofn.nMaxFile = MAX_PATH;
					ofn.Flags = OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT;
					if(GetSaveFileName(&ofn)) {
						UpdateRecordDialogPath(hwndDlg,szChoice);
					}
				}
				return TRUE;
			}
		}
	}

	return FALSE;
}

//Show the record movie dialog and record a movie.
void FCEUD_MovieRecordTo()
{
	if (!GameInfo) return;
	static struct CreateMovieParameters p;
	p.szFilename = strdup(FCEU_MakeFName(FCEUMKF_MOVIE,0,0).c_str());
	if(p.recordFrom >= 3) p.recordFrom=1;

	if(DialogBoxParam(fceu_hInstance, "IDD_RECORDINP", hAppWnd, RecordDialogProc, (LPARAM)&p))
	{
		if(p.recordFrom >= 3)
		{
			// attempt to load the savestate
			// FIXME:  pop open a messagebox if this fails
			FCEUI_LoadState(p.szSavestateFilename.c_str());
			{
				extern int loadStateFailed;

				if(loadStateFailed)
				{
					char str [1024];
					sprintf(str, "Failed to load save state \"%s\".\nRecording from current state instead...", p.szSavestateFilename.c_str());
					FCEUD_PrintError(str);
				}
			}
		}

		EMOVIE_FLAG flags = MOVIE_FLAG_NONE;
		if(p.recordFrom == 0) flags = MOVIE_FLAG_FROM_POWERON;
		if(p.recordFrom == 2) flags = MOVIE_FLAG_FROM_SAVERAM;
		FCEUI_SaveMovie(p.szFilename.c_str(), flags, p.authorName);
	}
}


void Replay_LoadMovie()
{
	if (suggestReadOnlyReplay)
		replayReadOnlySetting = true;
	else
		replayReadOnlySetting = FCEUI_GetMovieToggleReadOnly();

	char* fn = (char*)DialogBoxParam(fceu_hInstance, "IDD_REPLAYINP", hAppWnd, ReplayDialogProc, 0);

	if(fn)
	{
		FCEUI_LoadMovie(fn, replayReadOnlySetting, replayStopFrameSetting);

		free(fn);

		//mbg 6/21/08 - i think this stuff has to get updated in case the movie changed the pal emulation flag
		pal_emulation = FCEUI_GetCurrentVidSystem(0,0);
		UpdateCheckedMenuItems();
		SetMainWindowStuff();
		RefreshThrottleFPS();
	}
}

/// Show movie replay dialog and replay the movie if necessary.
void FCEUD_MovieReplayFrom()
{
	if (GameInfo) Replay_LoadMovie();
}
