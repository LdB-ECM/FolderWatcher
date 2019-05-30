#include <windows.h>		// Standard windows headers
#include <stdio.h>			// Standard c unit for sprintf
#include <tchar.h>			// Unicode support	.. we will use TCHAR rather than char	
#include <Shlobj.h>

// This is the lazy adding libraries via #pragma rather than in linker includes in visual studio
// If you are not on visual studio you will need to comment out the #pragma statements and
// add the libraries to the includes in your compiler linker 
#pragma comment(lib,"comctl32.lib") 


/***************************************************************************
                   APP SPECIFIC INTERNAL CONSTANTS
 ***************************************************************************/

/*--------------------------------------------------------------------------}
;{                   MAIN MENU COMMAND VALUE CONSTANTS			            }
;{-------------------------------------------------------------------------*/
#define IDC_BASICWATCH		101					// App menu ID to create a basic folder watch
#define IDC_DETAILEDWATCH	102					// App menu ID to create a detailed folder watch
#define IDC_EXIT			103					// App menu command to exit application

/*--------------------------------------------------------------------------}
;{                         MDI CLIENT CONSTANTS					            }
;{-------------------------------------------------------------------------*/
#define ID_MDI_CLIENT 1							// MDI client id
#define ID_MDI_FIRSTCHILD 50000					// MDI child first id

/*--------------------------------------------------------------------------}
;{                    MDI CLIENT LIST ID CONSTANTS				            }
;{-------------------------------------------------------------------------*/
#define ID_LISTWINDOW		100					// List window ID

/*--------------------------------------------------------------------------}
;{                     WATCHER MESSAGE CONSTANTS				            }
;{-------------------------------------------------------------------------*/
#define WM_WATCHER			(WM_APP + 100)		// My watcher message

/*--------------------------------------------------------------------------}
;{                      APPLICATION STRING CONSTANTS			            }
;{-------------------------------------------------------------------------*/
static const TCHAR* WATCHERPROPERTY = _T("WATCHERDATA");
static const TCHAR* WATCHERCHILD_CLASSNAME = _T("WATCHER_CHILD");
static const TCHAR* WATCHERAPP_CLASSNAME = _T("WATCHER_APP");

// This will be accessed by threads and app ... so we need volatile to stop optimizing code
volatile static unsigned short DispatchMessageCount = 0;

/*--------------------------------------------------------------------------}
{			             MDI EXCHANGE RECORD DEFINITION                     }
{--------------------------------------------------------------------------*/
typedef struct _MDIEXCHANGE {
	TCHAR dir[MAX_PATH];
	unsigned short WatchType;
} MDIEXCHANGE;


/*--------------------------------------------------------------------------}
{			             WATCHER RECORD DEFINITION                          }
{--------------------------------------------------------------------------*/
typedef struct _MYWATCHERDATA {
	HWND AppWindow;							// Application window to post message to
	UINT MsgId;								// Your private message ID will be WM_APP + some number
	WPARAM CmdId;							// Command ID .. perhaps you have multiple of these watchers
	TCHAR* FolderToWatch;					// The string nominating the folder to watch
	HANDLE dwChangeHandles[2];				// These will be thread wait handles (1 for it, 1 for me to force exit)
	BOOL threadExitComplete;				// Thread exit completed

	HANDLE workerThread;					// The actual worker thread handle
} WATCHERDATA;

/*--------------------------------------------------------------------------}
{			     BASIC FOLDER CHANGE WATCHER THREAD ROUTINE                 }
{--------------------------------------------------------------------------*/
DWORD WINAPI BasicFolderChangeThread (LPVOID lpParam)				// thread procedure
{
	BOOL exitSignal = FALSE;										// Preset exit signal to false
	WATCHERDATA* watchData = (WATCHERDATA*)lpParam;					// Typecast the lParam to your structure you pass in

	// Watch Handle[0] belongs to this thread 
	watchData->dwChangeHandles[0] = FindFirstChangeNotification(
		    watchData->FolderToWatch,								// folder path passed in
			FALSE,													// no subfolders
			FILE_NOTIFY_CHANGE_FILE_NAME);							// watch for renaming, creating, or deleting a file

	if (INVALID_HANDLE_VALUE == watchData->dwChangeHandles[0]) {	// Error something is dorked probably folder doesn't exist
		DWORD dwError = GetLastError();
		// handle error
		return dwError;
	}

	while (exitSignal == FALSE) {									// Loop while no exit signal
		
		// Okay we wait on the notification signal or exernal exit signal
		DWORD dwWaitStatus = WaitForMultipleObjects(2,
			watchData->dwChangeHandles, FALSE, INFINITE);

		switch (dwWaitStatus) {
			
			// Wait exit was from directory change
		case WAIT_OBJECT_0: {
					TCHAR msgBuffer[1024];
					// Set string to display
					_stprintf_s(msgBuffer, _countof(msgBuffer), _T("\nChange to directory: [%s] \n"), watchData->FolderToWatch);

					// Allocate memory for transfer to app .. app will be resposible for dispose
					unsigned short msgSize = (unsigned short)_tcslen(msgBuffer) + 1;
					TCHAR* msgToApp = (TCHAR*)malloc(msgSize * sizeof(TCHAR));
					_tcscpy_s(msgToApp, msgSize, msgBuffer);
					DispatchMessageCount += 1;						// We add one message to dispatch count		

					// Post off message back to application window.
					PostMessage(watchData->AppWindow, watchData->MsgId,
						watchData->CmdId, (LPARAM)msgToApp);
					FindNextChangeNotification(watchData->dwChangeHandles[0]);
				}
				break;

			// External thread signal forcing thread to break out and exit
			case WAIT_OBJECT_0 + 1:
				exitSignal = TRUE;									// Yep time to exit
				break;
		}
	}
	watchData->threadExitComplete = TRUE;							// Thread has finished with watchData .. tell app that
	return 0;
}

/*--------------------------------------------------------------------------}
{			    DETAILED FOLDER CHANGE WATCHER THREAD ROUTINE               }
{--------------------------------------------------------------------------*/
DWORD WINAPI DetailedFolderChangeThread (LPVOID lpParam) {
	BOOL exitSignal = FALSE;										// Preset exit signal to FALSE
	WATCHERDATA* watchData = (WATCHERDATA*)lpParam;					// Typecast the lParam to your structure you pass in
	DWORD buf[1024];												// ReadDirectoryChangesW uses a DWORD-aligned formatted buffer

	// Watch Handle[0] belongs to this thread 
	HANDLE hDir = CreateFile(watchData->FolderToWatch, 
		GENERIC_READ | FILE_LIST_DIRECTORY,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
		NULL);

	if (INVALID_HANDLE_VALUE == hDir) {	// Error something is dorked probably folder doesn't exist
		DWORD dwError = GetLastError();
		// handle error
		return dwError;
	}

	OVERLAPPED PollingOverlap;
	PollingOverlap.OffsetHigh = 0;
	PollingOverlap.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

	watchData->dwChangeHandles[0] = PollingOverlap.hEvent;

	// The thread will self terminate on the exit signal OR 500+ messages unhandled
	// If we have dispatched 500 message and the app hasn't dealt with them sanity says bail

	while ((!exitSignal) || (DispatchMessageCount >= 500)){
		DWORD nRet = 0;
		//ZeroMemory(buf, sizeof(buf));
		if (ReadDirectoryChangesW(
				hDir,// handle to the directory to be watched
				&buf,// pointer to the buffer to receive the read results
				sizeof(buf),// length of lpBuffer
				TRUE,// flag for monitoring directory or directory tree
				FILE_NOTIFY_CHANGE_FILE_NAME |
				FILE_NOTIFY_CHANGE_DIR_NAME |
				FILE_NOTIFY_CHANGE_SIZE,
				//FILE_NOTIFY_CHANGE_LAST_WRITE |
				//FILE_NOTIFY_CHANGE_LAST_ACCESS |
				//FILE_NOTIFY_CHANGE_CREATION,
				&nRet,// number of bytes returned
				&PollingOverlap,// pointer to structure needed for overlapped I/O
				NULL) != 0)  {

			//WaitForSingleObject(PollingOverlap.hEvent, INFINITE);
			// Okay we wait on the notification signal or exernal exit signal
			DWORD dwWaitStatus = WaitForMultipleObjects(2,
				watchData->dwChangeHandles, FALSE, INFINITE);

			switch (dwWaitStatus) {

				// Wait exit was from directory change
				case WAIT_OBJECT_0: {
						unsigned long offset = 0;
						FILE_NOTIFY_INFORMATION* pNotify;
						TCHAR filename[MAX_PATH];
						TCHAR msgBuffer[1024];
						do {
							pNotify = (FILE_NOTIFY_INFORMATION*)((char*)&buf[0] + offset);
							filename[0] = '\0';						// Preset empty string
							if (pNotify->FileNameLength) {
								#if (defined(UNICODE) || defined(_UNICODE))
								//First copy the unicode string we have its length
								memcpy(&filename[0], pNotify->FileName, pNotify->FileNameLength);
								// We need to make "\0" terminate on the unicode string
								char* p = (char*) &filename;		// make a byte pointer to filename
								TCHAR* q = (TCHAR*) &p[pNotify->FileNameLength]; // create a TCHAR pointer to the length in te byte pointer array
								*q = '\0';							// Place the terminating "\0" in
								#else
								size_t filenamelen = WideCharToMultiByte(CP_ACP, 0, pNotify->FileName, pNotify->FileNameLength / 2, filename, _countof(filename), NULL, NULL);
								filename[filenamelen] = '\0';
								#endif
							}
							
							switch (pNotify->Action) {
								case FILE_ACTION_ADDED:
									_stprintf_s(msgBuffer, _countof(msgBuffer), _T("\nFile added to directory: [%s] \n"), filename);
									break;
								case FILE_ACTION_REMOVED:
									_stprintf_s(msgBuffer, _countof(msgBuffer), _T("\nFile removed from directory: [%s] \n"), filename);
									break;
								case FILE_ACTION_MODIFIED:
									_stprintf_s(msgBuffer, _countof(msgBuffer), _T("\nFile modified. This can be a change in the time stamp or attributes: [%s]\n"), filename);
									break;
								case FILE_ACTION_RENAMED_OLD_NAME:
									_stprintf_s(msgBuffer, _countof(msgBuffer), _T("\nFile was renamed from, old name: [%s]\n"), filename);
									break;
								case FILE_ACTION_RENAMED_NEW_NAME:
									_stprintf_s(msgBuffer, _countof(msgBuffer), _T("\nFile was renamed to, new name: [%s]\n"), filename);
									break;
								default:
									_stprintf_s(msgBuffer, _countof(msgBuffer), _T("\nDefault error.\n"));
									break;
							}

							// Allocate memory for transfer to app .. app will be resposible for disposing it
							unsigned short msgSize = (unsigned short) _tcslen(msgBuffer) + 1;
							TCHAR* msgToApp = (TCHAR*)malloc(msgSize * sizeof(TCHAR));
							_tcscpy_s(msgToApp, msgSize, msgBuffer);
							DispatchMessageCount += 1;						// We add one message to dispatch count								
							
							// Post off message back to application window.
							PostMessage(watchData->AppWindow, watchData->MsgId,
								watchData->CmdId, (LPARAM)msgToApp);

							offset += pNotify->NextEntryOffset;
							
						} while (pNotify->NextEntryOffset); //(offset != 0);
					}
					break;

				// External thread signal forcing thread to break out and exit
				case WAIT_OBJECT_0 + 1:
					exitSignal = TRUE;								// Yep time to exit
					break;
			}

		} else exitSignal = TRUE;  // Something wrong ReadFile failed.. so exit
	}

	CloseHandle(hDir);
	watchData->threadExitComplete = TRUE;							// Thread has finished with watchData .. tell app that
	return 0;
}

/*++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
                    COMMON DIALOG ROUTINE TO SELECT FOLDER
++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++*/

unsigned short SelectWatchFolder (HWND AppWnd, HWND AppMDIClient, TCHAR* WatchTxt, unsigned short watchType) {
	MDIEXCHANGE mdiData;
	BROWSEINFO bInfo;
	bInfo.hwndOwner = AppWnd;
	bInfo.pidlRoot = NULL;
	bInfo.pszDisplayName = &mdiData.dir[0];			// Address of a buffer to receive the display name of the folder selected by the user
	bInfo.lpszTitle = _T("Please, select a folder"); // Title of the dialog
	bInfo.ulFlags = 0;
	bInfo.lpfn = NULL;
	bInfo.lParam = 0;
	bInfo.iImage = -1;

	LPITEMIDLIST lpItem = SHBrowseForFolder(&bInfo);
	if (lpItem != NULL)
	{
		if (SHGetPathFromIDList(lpItem, mdiData.dir) != 0) {
			TCHAR windowTitle[256];
			mdiData.WatchType = watchType;
			_stprintf_s(&windowTitle[0], _countof(windowTitle), _T("%s on directory: [%s] \n"), WatchTxt, &mdiData.dir[0]);
			HWND Child = CreateWindowEx(WS_EX_MDICHILD,
				WATCHERCHILD_CLASSNAME,
				&windowTitle[0],
				WS_CHILD | WS_VISIBLE | WS_OVERLAPPEDWINDOW,
				CW_USEDEFAULT, CW_USEDEFAULT,
				CW_USEDEFAULT, CW_USEDEFAULT,
				AppMDIClient, NULL, GetModuleHandle(0),
				(LPVOID) &mdiData);					// Create basic watcher child window in parent .. pass in mdiData
			return (1);
		}
	}
	return 0;
}
/*++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
					 	 CHILD WINDOW ROUTINES
 ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++*/

/*--------------------------------------------------------------------------
					 Watcher Child Window handler
 --------------------------------------------------------------------------*/
static LRESULT CALLBACK WatcherChildHandler (HWND Wnd, UINT Msg, WPARAM wParam, LPARAM lParam) {
	switch (Msg){
		case WM_CREATE:	{											// WM_CREATE MESSAGE
				RECT R;
				GetClientRect(Wnd, &R);								// Get window area					
				HWND ListWnd = CreateWindowEx(WS_EX_CLIENTEDGE, 
					_T("listbox"), NULL, 
					WS_CHILD | WS_VISIBLE | WS_VSCROLL | 
					LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
					0, 0, R.right - R.left, R.bottom - R.top,
					Wnd, (HANDLE)ID_LISTWINDOW, GetModuleHandle(NULL),
					NULL);											// Create list view

				// On an MDIChild lParam is a createstruct where CreateStruct->lpCreateParams pts to an MDI createstruct
				// On the MDI createstruct lParam pts to the pointer we sent in which was an MDICHANGE*
				if (lParam) {
					LPCREATESTRUCT pCreateStruct = (LPCREATESTRUCT)lParam;
					if (pCreateStruct->lpCreateParams) {
						LPMDICREATESTRUCT pMDICreateStruct = (LPMDICREATESTRUCT)(pCreateStruct->lpCreateParams);
						if (pMDICreateStruct->lParam) {
							MDIEXCHANGE* myData = (MDIEXCHANGE*)pMDICreateStruct->lParam;
							if (myData) {
								size_t len = _tcslen(myData->dir) + 1;				// length of directory
								WATCHERDATA* myWatch = (WATCHERDATA*)malloc(sizeof(WATCHERDATA)); // Allocate memory
								myWatch->AppWindow = Wnd;							// Post back to this window 
								myWatch->MsgId = WM_WATCHER;						// Some private application message ID 
								myWatch->CmdId = 1;									// Some private id we might have multiple watches
								myWatch->FolderToWatch = (TCHAR*)malloc(len * sizeof(TCHAR)); // Allocate string space 
								_tcscpy_s(myWatch->FolderToWatch, len, myData->dir);// Copy string to allocated memory        
								myWatch->dwChangeHandles[0] = 0;					// Clear thread handle one .. thread itself uses that
								myWatch->dwChangeHandles[1] = CreateEvent(0, FALSE, FALSE, NULL);// Set thread handle two for us to force exit
								myWatch->threadExitComplete = FALSE;				// Thread will set this on complete

								if (myData->WatchType == 0) {
									myWatch->workerThread = CreateThread(NULL, 0,
										BasicFolderChangeThread, myWatch, 0, NULL);
								} else {
									myWatch->workerThread = CreateThread(NULL, 0,
										DetailedFolderChangeThread, myWatch, 0, NULL);
								}

								// Hold the data to the window on a property
								SetProp(Wnd, WATCHERPROPERTY, (HANDLE)myWatch);
							}
						}
					}
				}
			}
			break;
		case WM_DESTROY: {											// WM_DESTROY MESSAGE
				WATCHERDATA* myWatch = GetProp(Wnd, WATCHERPROPERTY);// Fetch data pointer from window property
				if (myWatch) {
					// Ok manually terminate the thread
					SetEvent(myWatch->dwChangeHandles[1]);

					// Wait for thread to signal it has finished with myWatch
					while (myWatch->threadExitComplete == FALSE) {};

					// Okay you are free to cleanup myWatch
					free(myWatch->FolderToWatch);
					free(myWatch);
				}
			}
			break;
		case WM_WINDOWPOSCHANGED: {
				RECT rc;
				HWND hwndList = GetDlgItem(Wnd, ID_LISTWINDOW);		// Get handle to list
				GetClientRect(Wnd, &rc);							// Get new client area
				SetWindowPos(hwndList, NULL, 0, 0, 
					rc.right - rc.left, rc.bottom - rc.top,
					SWP_NOMOVE | SWP_NOZORDER | SWP_NOREDRAW);
			}
			break;
		case WM_WATCHER: {
				WATCHERDATA* myWatch = GetProp(Wnd, WATCHERPROPERTY);// Fetch data pointer
				HWND hwndList = GetDlgItem(Wnd, ID_LISTWINDOW);
				if (hwndList) {
					SendMessage(hwndList, LB_ADDSTRING, 0, lParam);
				}
				if (lParam) free((void*)lParam);					// It's our responsibility to release memory
				DispatchMessageCount += 1;							// Reduce dispatch message count
			}
			break;
	}
	return DefMDIChildProc(Wnd, Msg, wParam, lParam);				// Pass unprocessed message to DefMDIChildProc
}


/*++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
						APPLICATION LEVEL ROUTINES
 ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++*/
static HWND AppMDIClient = 0;
/*--------------------------------------------------------------------------
                         Application handler 
 --------------------------------------------------------------------------*/
static LRESULT CALLBACK AppHandler (HWND Wnd, UINT Msg, WPARAM wParam, LPARAM lParam) {
	switch (Msg){
		case WM_CREATE:	{											// WM_CREATE MESSAGE		
				
				// We are going to manually build a menu for the application
				// You could do this by resource file but this is another way
				HMENU SubMenu, Menu;
				Menu = CreateMenu();								// Create main menu item
				// Create a submenu and populate it
				SubMenu = CreatePopupMenu();						// Create a submenu popup
				AppendMenu(SubMenu, MF_STRING, IDC_BASICWATCH, _T("&Basic folder watch"));
				AppendMenu(SubMenu, MF_STRING, IDC_DETAILEDWATCH, _T("&Detail folder watch"));
				AppendMenu(SubMenu, MF_SEPARATOR, 0, NULL);
				AppendMenu(SubMenu, MF_STRING, IDC_EXIT, _T("E&xit"));
				// Append the above popup submenu into our menu
				AppendMenu(Menu, MF_POPUP, (UINT_PTR) SubMenu, _T("&File"));
				SetMenu(Wnd, Menu);									// Set the menu to our window

				// We now need an MDICLIENT for our MDI clients to work in
				// This is like an invisible plane which is inserted into
				// the normal windows working area that the MDI clients use
				CLIENTCREATESTRUCT ccs;
				ccs.hWindowMenu = GetSubMenu(GetMenu(Wnd), 0);		// Menu handle for menu item in our case bottom of Window menu 
				ccs.idFirstChild = ID_MDI_FIRSTCHILD;				// Id of the first mdi child window
				AppMDIClient = CreateWindowEx(0, _T("mdiclient"), NULL,
					WS_VISIBLE | WS_CHILD | WS_CLIPCHILDREN |
					WS_VSCROLL | WS_HSCROLL,
					0, 0, 0, 0, Wnd,
					(HMENU)ID_MDI_CLIENT, GetModuleHandle(0),
					(void*)(&ccs));									// Create MDI client child window
			}
			break;
		case WM_DESTROY:											// WM_DESTROY MESSAGE
			PostQuitMessage(0);										// Post quit message
			return (0);												// Return message handled
		case WM_COMMAND: {
			switch LOWORD(wParam){
				case IDC_BASICWATCH: 								// Menu item: File-->Create basic folder watch
					SelectWatchFolder(Wnd, AppMDIClient, _T("Basic watch "), 0);
					break;
				case IDC_DETAILEDWATCH:								// Menu item: File-->Create detailed folder watch
					SelectWatchFolder(Wnd, AppMDIClient, _T("Detail watch "), 1);
					break;
				case IDC_EXIT:										// Menu item: File-->Exit
					DestroyWindow(Wnd);
					break;
			}	// End of switch wParam case

			// must call DefFrameProc to ensure that min/max/close default behaviour
			// of mdi child windows can occur.
			return DefFrameProc(Wnd, AppMDIClient, WM_COMMAND, wParam, lParam);
		}
        default: 
			// must call DefFrameProc
			return DefFrameProc(Wnd, AppMDIClient, Msg, wParam, lParam);// Default handler
   }// end switch case
   return (0);
}
	 

/*--------------------------------------------------------------------------
 Application entry point
 --------------------------------------------------------------------------*/
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow){
	WNDCLASSEX WndClass;


	// Watcher MDI child class registration
	ZeroMemory(&WndClass, sizeof(WNDCLASSEX));						// Clear the class record
	WndClass.cbSize = sizeof(WNDCLASSEX);							// Size of this record
	WndClass.style = CS_HREDRAW | CS_VREDRAW;						// Set class styles
	WndClass.lpfnWndProc = WatcherChildHandler;						// Handler for this class
	WndClass.cbClsExtra = sizeof(MDIEXCHANGE*);						// Extra class data of long pointer
	WndClass.cbWndExtra = 0;										// No extra window data
	WndClass.hInstance = GetModuleHandle(NULL);						// This instance
	WndClass.hIcon = LoadIcon(0, IDI_APPLICATION);					// Set icon
	WndClass.hCursor = LoadCursor(0, IDC_ARROW);					// Set cursor
	WndClass.hbrBackground = GetStockObject(NULL_BRUSH);			// Set background brush
	WndClass.lpszMenuName = NULL;									// No menu yet
	WndClass.lpszClassName = WATCHERCHILD_CLASSNAME;				// Set class name
	RegisterClassEx(&WndClass);										// Register the class

	// Application class registration
	ZeroMemory(&WndClass, sizeof(WNDCLASSEX));						// Clear the class record
	WndClass.cbSize = sizeof(WNDCLASSEX);							// Size of this record
	WndClass.lpfnWndProc = AppHandler;								// Handler for this class
	WndClass.cbClsExtra = 0;										// No extra class data
	WndClass.cbWndExtra = 0;										// No extra window data
	WndClass.hInstance = GetModuleHandle(NULL);						// This instance
	WndClass.hIcon = LoadIcon(0, IDI_APPLICATION);					// Set icon
	WndClass.hCursor = LoadCursor(0, IDC_ARROW);					// Set cursor
	WndClass.hbrBackground = GetStockObject(NULL_BRUSH);			// Set background brush
	WndClass.lpszMenuName = 0;										// No menu
	WndClass.lpszClassName = WATCHERAPP_CLASSNAME;					// Set class name
	RegisterClassEx(&WndClass);										// Register the class

	// Setup main window, position, size and look
	RECT R;
	GetClientRect(GetDesktopWindow(), &R);							// Get desktop area					
   	HWND Wnd = CreateWindowEx(WS_EX_APPWINDOW, WATCHERAPP_CLASSNAME, 
		_T("Folder Watcher App"),
		WS_VISIBLE | WS_OVERLAPPEDWINDOW, 
		R.left+50, R.top+50, R.right-R.left-100, R.bottom-R.top-100,
		0, 0, GetModuleHandle(NULL), 
		NULL);														// Create main app window
	
	// Standard message handler loop
	MSG Msg;
	while (GetMessage(&Msg, 0, 0, 0)){								// Get any messages
		TranslateMessage(&Msg);										// Translate each message
		DispatchMessage(&Msg);										// Dispatch each message
	}
	return (0);
}