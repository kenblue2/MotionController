﻿//------------------------------------------------------------------------------
// <copyright file="SkeletonBasics.cpp" company="Microsoft">
//     Copyright (c) Microsoft Corporation.  All rights reserved.
// </copyright>
//------------------------------------------------------------------------------

#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include "stdafx.h"
#include <strsafe.h>
#include "SkeletonBasics.h"
#include "resource.h"

#include <mmsystem.h>
#include <WinSock2.h>
#include <process.h>
#include <vector>
#include <algorithm>

#pragma comment (lib, "ws2_32.lib")
#pragma comment (lib, "winmm.lib")

SOCKET clnt_sock;

//------------------------------------------------------------------------------
void __cdecl RecvThread(void * p)
{
	SOCKET sock = (SOCKET)p;
	char buf[256];
	while (true)
	{
		int recvsize = recv(sock, buf, sizeof(buf), 0);
		if (recvsize <= 0)
		{
			printf("접속종료\n");
			break;
		}
		buf[recvsize] = '\0';
		printf("\rclient >> %s\n>> ", buf);
	}
}


static const float g_JointThickness = 3.0f;
static const float g_TrackedBoneThickness = 6.0f;
static const float g_InferredBoneThickness = 1.0f;

/// <summary>
/// Entry point for the application
/// </summary>
/// <param name="hInstance">handle to the application instance</param>
/// <param name="hPrevInstance">always 0</param>
/// <param name="lpCmdLine">command line arguments</param>
/// <param name="nCmdShow">whether to display minimized, maximized, or normally</param>
/// <returns>status</returns>
int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow)
{
	WSADATA wsaData;
	int retval = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (retval != 0)
	{
		printf("WSAStartup() Error\n");
		return 0;
	}

	SOCKET serv_sock;
	serv_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (serv_sock == SOCKET_ERROR)
	{
		printf("socket() Error\n");
		return 0;
	}

	SOCKADDR_IN serv_addr = { 0 };
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_port = htons(4000);
	serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);

	retval = bind(serv_sock, (SOCKADDR*)&serv_addr, sizeof(SOCKADDR));
	if (retval == SOCKET_ERROR)
	{
		printf("bind() Error\n");
		return 0;
	}

	listen(serv_sock, 5);

	SOCKADDR_IN clnt_addr = { 0 };
	int size = sizeof(SOCKADDR_IN);
	//SOCKET clnt_sock = accept(serv_sock, (SOCKADDR*)&clnt_addr, &size);
	clnt_sock = accept(serv_sock, (SOCKADDR*)&clnt_addr, &size);
	if (clnt_sock == SOCKET_ERROR)
	{
		printf("accept() Error\n");
		return 0;
	}
	printf("클라이언트 접속\n");
	printf("IP : %s, Port : %d\n", inet_ntoa(clnt_addr.sin_addr), clnt_addr.sin_port);
	HANDLE hThread = (HANDLE)_beginthread(RecvThread, NULL, (void*)clnt_sock);

	// main
	{
		CSkeletonBasics application;
		application.Run(hInstance, nCmdShow);
	}
	
	closesocket(clnt_sock);
	closesocket(serv_sock);
	WSACleanup();
	
	return TRUE;
}

/// <summary>
/// Constructor
/// </summary>
CSkeletonBasics::CSkeletonBasics() :
    m_pD2DFactory(NULL),
    m_hNextSkeletonEvent(INVALID_HANDLE_VALUE),
    m_pSkeletonStreamHandle(INVALID_HANDLE_VALUE),
    m_bSeatedMode(false),
    m_pRenderTarget(NULL),
    m_pBrushJointTracked(NULL),
    m_pBrushJointInferred(NULL),
    m_pBrushBoneTracked(NULL),
    m_pBrushBoneInferred(NULL),
    m_pNuiSensor(NULL)
{
    ZeroMemory(m_Points,sizeof(m_Points));
}

/// <summary>
/// Destructor
/// </summary>
CSkeletonBasics::~CSkeletonBasics()
{
    if (m_pNuiSensor)
    {
        m_pNuiSensor->NuiShutdown();
    }

    if (m_hNextSkeletonEvent && (m_hNextSkeletonEvent != INVALID_HANDLE_VALUE))
    {
        CloseHandle(m_hNextSkeletonEvent);
    }

    // clean up Direct2D objects
    DiscardDirect2DResources();

    // clean up Direct2D
    SafeRelease(m_pD2DFactory);

    SafeRelease(m_pNuiSensor);
}

/// <summary>
/// Creates the main window and begins processing
/// </summary>
/// <param name="hInstance">handle to the application instance</param>
/// <param name="nCmdShow">whether to display minimized, maximized, or normally</param>
int CSkeletonBasics::Run(HINSTANCE hInstance, int nCmdShow)
{
    MSG       msg = {0};
    WNDCLASS  wc  = {0};

    // Dialog custom window class
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.cbWndExtra    = DLGWINDOWEXTRA;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon         = LoadIconW(hInstance, MAKEINTRESOURCE(IDI_APP));
    wc.lpfnWndProc   = DefDlgProcW;
    wc.lpszClassName = L"SkeletonBasicsAppDlgWndClass";

    if (!RegisterClassW(&wc))
    {
        return 0;
    }

    // Create main application window
    HWND hWndApp = CreateDialogParamW(
        hInstance,
        MAKEINTRESOURCE(IDD_APP),
        NULL,
        (DLGPROC)CSkeletonBasics::MessageRouter, 
        reinterpret_cast<LPARAM>(this));

    // Show window
    ShowWindow(hWndApp, nCmdShow);

    const int eventCount = 1;
    HANDLE hEvents[eventCount];

    // Main message loop
    while (WM_QUIT != msg.message)
    {
        hEvents[0] = m_hNextSkeletonEvent;

        // Check to see if we have either a message (by passing in QS_ALLEVENTS)
        // Or a Kinect event (hEvents)
        // Update() will check for Kinect events individually, in case more than one are signalled
        MsgWaitForMultipleObjects(eventCount, hEvents, FALSE, INFINITE, QS_ALLINPUT);

        // Explicitly check the Kinect frame event since MsgWaitForMultipleObjects
        // can return for other reasons even though it is signaled.
        Update();

        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            // If a dialog message will be taken care of by the dialog proc
            if ((hWndApp != NULL) && IsDialogMessageW(hWndApp, &msg))
            {
                continue;
            }

            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    return static_cast<int>(msg.wParam);
}

/// <summary>
/// Main processing function
/// </summary>
void CSkeletonBasics::Update()
{
    if (NULL == m_pNuiSensor)
    {
        return;
    }

    // Wait for 0ms, just quickly test if it is time to process a skeleton
    if ( WAIT_OBJECT_0 == WaitForSingleObject(m_hNextSkeletonEvent, 0) )
    {
        ProcessSkeleton();
    }
}

/// <summary>
/// Handles window messages, passes most to the class instance to handle
/// </summary>
/// <param name="hWnd">window message is for</param>
/// <param name="uMsg">message</param>
/// <param name="wParam">message data</param>
/// <param name="lParam">additional message data</param>
/// <returns>result of message processing</returns>
LRESULT CALLBACK CSkeletonBasics::MessageRouter(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    CSkeletonBasics* pThis = NULL;

    if (WM_INITDIALOG == uMsg)
    {
        pThis = reinterpret_cast<CSkeletonBasics*>(lParam);
        SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
    }
    else
    {
        pThis = reinterpret_cast<CSkeletonBasics*>(::GetWindowLongPtr(hWnd, GWLP_USERDATA));
    }

    if (pThis)
    {
        return pThis->DlgProc(hWnd, uMsg, wParam, lParam);
    }

    return 0;
}

/// <summary>
/// Handle windows messages for the class instance
/// </summary>
/// <param name="hWnd">window message is for</param>
/// <param name="uMsg">message</param>
/// <param name="wParam">message data</param>
/// <param name="lParam">additional message data</param>
/// <returns>result of message processing</returns>
LRESULT CALLBACK CSkeletonBasics::DlgProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_INITDIALOG:
        {
            // Bind application window handle
            m_hWnd = hWnd;

            // Init Direct2D
            D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &m_pD2DFactory);

            // Look for a connected Kinect, and create it if found
            CreateFirstConnected();
        }
        break;

        // If the titlebar X is clicked, destroy app
    case WM_CLOSE:
        DestroyWindow(hWnd);
        break;

    case WM_DESTROY:
        // Quit the main message pump
        PostQuitMessage(0);
        break;

        // Handle button press
    case WM_COMMAND:
        // If it was for the near mode control and a clicked event, change near mode
        if (IDC_CHECK_SEATED == LOWORD(wParam) && BN_CLICKED == HIWORD(wParam))
        {
            // Toggle out internal state for near mode
            m_bSeatedMode = !m_bSeatedMode;

            if (NULL != m_pNuiSensor)
            {
                // Set near mode for sensor based on our internal state
                m_pNuiSensor->NuiSkeletonTrackingEnable(m_hNextSkeletonEvent, m_bSeatedMode ? NUI_SKELETON_TRACKING_FLAG_ENABLE_SEATED_SUPPORT : 0);
            }
        }
        break;
    }

    return FALSE;
}

/// <summary>
/// Create the first connected Kinect found 
/// </summary>
/// <returns>indicates success or failure</returns>
HRESULT CSkeletonBasics::CreateFirstConnected()
{
    INuiSensor * pNuiSensor;

    int iSensorCount = 0;
    HRESULT hr = NuiGetSensorCount(&iSensorCount);
    if (FAILED(hr))
    {
        return hr;
    }

    // Look at each Kinect sensor
    for (int i = 0; i < iSensorCount; ++i)
    {
        // Create the sensor so we can check status, if we can't create it, move on to the next
        hr = NuiCreateSensorByIndex(i, &pNuiSensor);
        if (FAILED(hr))
        {
            continue;
        }

        // Get the status of the sensor, and if connected, then we can initialize it
        hr = pNuiSensor->NuiStatus();
        if (S_OK == hr)
        {
            m_pNuiSensor = pNuiSensor;
            break;
        }

        // This sensor wasn't OK, so release it since we're not using it
        pNuiSensor->Release();
    }

    if (NULL != m_pNuiSensor)
    {
        // Initialize the Kinect and specify that we'll be using skeleton
        hr = m_pNuiSensor->NuiInitialize(NUI_INITIALIZE_FLAG_USES_SKELETON); 
        if (SUCCEEDED(hr))
        {
            // Create an event that will be signaled when skeleton data is available
            m_hNextSkeletonEvent = CreateEventW(NULL, TRUE, FALSE, NULL);

            // Open a skeleton stream to receive skeleton data
            hr = m_pNuiSensor->NuiSkeletonTrackingEnable(m_hNextSkeletonEvent, 0); 
        }
    }

    if (NULL == m_pNuiSensor || FAILED(hr))
    {
        SetStatusMessage(L"No ready Kinect found!");
        return E_FAIL;
    }

    return hr;
}

/// <summary>
/// Handle new skeleton data
/// </summary>
void CSkeletonBasics::ProcessSkeleton()
{
    NUI_SKELETON_FRAME skeletonFrame = {0};

    HRESULT hr = m_pNuiSensor->NuiSkeletonGetNextFrame(0, &skeletonFrame);
    if ( FAILED(hr) )
    {
        return;
    }

	NUI_TRANSFORM_SMOOTH_PARAMETERS params;
	{
		params.fSmoothing = 0.5f;
		params.fCorrection = 0.5f;
		params.fPrediction = 0.5f;
		params.fJitterRadius = 0.5f;
		params.fMaxDeviationRadius = 0.5f;
	}

    // smooth out the skeleton data
    //m_pNuiSensor->NuiTransformSmooth(&skeletonFrame, NULL);
	m_pNuiSensor->NuiTransformSmooth(&skeletonFrame, &params);

    // Endure Direct2D is ready to draw
    hr = EnsureDirect2DResources( );
    if ( FAILED(hr) )
    {
        return;
    }

    m_pRenderTarget->BeginDraw();
    m_pRenderTarget->Clear( );

    RECT rct;
    GetClientRect( GetDlgItem( m_hWnd, IDC_VIDEOVIEW ), &rct);
    int width = rct.right;
    int height = rct.bottom;

    for (int i = 0 ; i < NUI_SKELETON_COUNT; ++i)
    {
        NUI_SKELETON_TRACKING_STATE trackingState = skeletonFrame.SkeletonData[i].eTrackingState;

        if (NUI_SKELETON_TRACKED == trackingState)
        {
            // We're tracking the skeleton, draw it
            DrawSkeleton(skeletonFrame.SkeletonData[i], width, height);
        }
        else if (NUI_SKELETON_POSITION_ONLY == trackingState)
        {
            // we've only received the center point of the skeleton, draw that
            D2D1_ELLIPSE ellipse = D2D1::Ellipse(
                SkeletonToScreen(skeletonFrame.SkeletonData[i].Position, width, height),
                g_JointThickness,
                g_JointThickness
                );

            m_pRenderTarget->DrawEllipse(ellipse, m_pBrushJointTracked);
        }
    }

    hr = m_pRenderTarget->EndDraw();

    // Device lost, need to recreate the render target
    // We'll dispose it now and retry drawing
    if (D2DERR_RECREATE_TARGET == hr)
    {
        hr = S_OK;
        DiscardDirect2DResources();
    }
}

const float SCALE = 100.f;

struct FVector
{
	FVector() {}
	FVector(float x, float y, float z)
		: X(x), Y(y), Z(z) {}

	FVector(const Vector4& pos)
	{
		X = -pos.z * SCALE;
		Y = pos.x * SCALE;
		Z = pos.y * SCALE + 90.f;
	}

	Vector4 Convert()
	{
		Vector4 vec;

		vec.x = Y / SCALE;
		vec.y = (Z - 90.f) / SCALE;
		vec.z = -X / SCALE;
		vec.w = 1;

		return vec;
	}

	FVector operator - (const FVector& vec) const
	{
		return FVector(X - vec.X, Y - vec.Y, Z - vec.Z);
	}

	float Size() const
	{
		return sqrtf(X * X + Y * Y + Z * Z);
	}

	float SizeYZ() const
	{
		return sqrtf(Y * Y + Z * Z);
	}

	float SizeXZ() const
	{
		return sqrtf(X * X + Z * Z);
	}

	float X, Y, Z;
};

const float POS_SCALE = 4.f;
const float VEL_SCALE = 15.f;
const float PASS_MIN_VEL = 20.f;
const float TOSS_MIN_VEL = 10.f;
const float ATTACK_MIN_HEIGHT = 90.f;

FVector BonePosList[NUI_SKELETON_POSITION_COUNT];

std::vector<FVector> RightFootVelList;
std::vector<FVector> RightFootPosList;
std::vector<D2D_POINT_2F> RightFootPosList2;

std::vector<FVector> LeftFootVelList;
std::vector<FVector> LeftFootPosList;
std::vector<D2D_POINT_2F> LeftFootPosList2;

FVector MotionVectors[5];
D2D_POINT_2F MotionVectors2[5];


struct FSendInfo
{
	bool bAttack;
	bool bPass;
	bool bToss;
	bool bUseLeftFoot;

	FVector MotionVectors[5];
};

FSendInfo SendInfo;

bool IsHigher(const std::vector<FVector>& PosList, float limitHeight)
{
	for (const FVector & pos : PosList)
	{
		if (pos.Z > limitHeight)
			return true;
	}

	return false;
}

bool IsFaster(const std::vector<FVector>& VelList, float limitSpeed)
{
	for (const FVector & vel : VelList)
	{
		if (vel.Size() > limitSpeed)
			return true;
	}

	return false;
}

bool SearchMotionVector(const std::vector<FVector>& PosList, const std::vector<FVector>& VelList, const std::vector<D2D_POINT_2F>& PosList2)
{
	if (PosList.empty() || VelList.empty())
		return false;

	int maxHeightFrame = -1;
	float maxHeight = 0.f;

	for (int i = 0; i < (int)PosList.size(); ++i)
	{
		if (maxHeight < PosList[i].Z)
		{
			maxHeight = PosList[i].Z;
			maxHeightFrame = i;
		}
	}


	int endFrame = -1;
	int beginFrame = -1;
	int maxSpeedFrame1 = -1;
	int maxSpeedFrame2 = -1;

	float maxSpeed = 0.f;
	for (int i = maxHeightFrame; i > 0; --i)
	{
		float speed = VelList[i].Size();

		if (maxSpeed < speed)
		{
			maxSpeed = speed;
			maxSpeedFrame1 = i;
		}
	}

	maxSpeed = 0.f;	
	for (int i = maxHeightFrame; i < (int)VelList.size(); ++i)
	{
		float speed = VelList[i].Size();

		if (maxSpeed < speed)
		{
			maxSpeed = speed;
			maxSpeedFrame2 = i;
		}
	}

	for (int i = maxSpeedFrame1; i > 0; --i)
	{
		float speed = VelList[i].Size();

		if (speed < 4.f)
		{
			beginFrame = i;
			break;
		}
	}

	for (int i = maxSpeedFrame2; i < (int)VelList.size(); ++i)
	{
		float speed = VelList[i].Size();

		if (speed < 4.f)
		{
			endFrame = i;
			break;
		}
	}

	if (endFrame > 0 && beginFrame >= 0 && maxHeightFrame > 0 && maxSpeedFrame1 > 0 && maxSpeedFrame2 > 0)
	{
		MotionVectors[0] = PosList[beginFrame];
		MotionVectors[1] = PosList[maxSpeedFrame1];
		MotionVectors[2] = PosList[maxHeightFrame];
		MotionVectors[3] = PosList[maxSpeedFrame2];
		MotionVectors[4] = PosList[endFrame];

		MotionVectors2[0] = PosList2[beginFrame];
		MotionVectors2[1] = PosList2[maxSpeedFrame1];
		MotionVectors2[2] = PosList2[maxHeightFrame];
		MotionVectors2[3] = PosList2[maxSpeedFrame2];
		MotionVectors2[4] = PosList2[endFrame];

		return true;
	}

	return false;
}


/// <summary>
/// Draws a skeleton
/// </summary>
/// <param name="skel">skeleton to draw</param>
/// <param name="windowWidth">width (in pixels) of output buffer</param>
/// <param name="windowHeight">height (in pixels) of output buffer</param>
void CSkeletonBasics::DrawSkeleton(const NUI_SKELETON_DATA & skel, int windowWidth, int windowHeight)
{   
	static DWORD beginTime = timeGetTime();

	DWORD deltaTime = timeGetTime() - beginTime;

	beginTime = timeGetTime();

    int i;

    for (i = 0; i < NUI_SKELETON_POSITION_COUNT; ++i)
    {
		Vector4 BonePos = skel.SkeletonPositions[i];
		BonePos.x = -BonePos.x;

		m_Points[i] = SkeletonToScreen(BonePos, windowWidth, windowHeight);

		BonePosList[i] = FVector(BonePos);
    }

	// right foot
	{
		FVector RightFootPos = BonePosList[NUI_SKELETON_POSITION_ANKLE_RIGHT];

		if (RightFootPosList.size() > 0)
		{
			FVector FootVel = RightFootPos - RightFootPosList.back();

			RightFootVelList.push_back(FootVel);
		}
		else
		{
			FVector FootVel(0, 0, 0);

			RightFootVelList.push_back(FootVel);
		}

		RightFootPosList.push_back(RightFootPos);
		RightFootPosList2.push_back(m_Points[NUI_SKELETON_POSITION_ANKLE_RIGHT]);

		if (RightFootPosList.size() > 100)
		{
			RightFootVelList.erase(RightFootVelList.begin());
			RightFootPosList.erase(RightFootPosList.begin());
			RightFootPosList2.erase(RightFootPosList2.begin());
		}
	}

	// left foot
	{
		FVector LeftFootPos = BonePosList[NUI_SKELETON_POSITION_ANKLE_LEFT];

		if (LeftFootPosList.size() > 0)
		{
			FVector FootVel = LeftFootPos - LeftFootPosList.back();

			LeftFootVelList.push_back(FootVel);
		}
		else
		{
			FVector FootVel(0, 0, 0);

			LeftFootVelList.push_back(FootVel);
		}

		LeftFootPosList.push_back(LeftFootPos);
		LeftFootPosList2.push_back(m_Points[NUI_SKELETON_POSITION_ANKLE_LEFT]);

		if (LeftFootPosList.size() > 100)
		{
			LeftFootVelList.erase(LeftFootVelList.begin());
			LeftFootPosList.erase(LeftFootPosList.begin());
			LeftFootPosList2.erase(LeftFootPosList2.begin());
		}
	}

	//WCHAR msg[512];
	//swprintf_s(msg, L"foot height: %f", FootPos.Y);
	//swprintf_s(msg, L"delta time: %d", deltaTime);

	//SetStatusMessage(msg);

	// draw right foot graph
	{
		float deltaX = float(windowWidth) / float(RightFootPosList.size());

		for (int idxPos = 1; idxPos < (int)RightFootPosList.size(); ++idxPos)
		{
			const FVector& FP1 = RightFootPosList[idxPos - 1];
			const FVector& FP2 = RightFootPosList[idxPos];

			D2D1_POINT_2F pos1;
			D2D1_POINT_2F pos2;

			pos1.x = float(idxPos - 1) * deltaX;
			pos2.x = float(idxPos) * deltaX;

			pos1.y = windowHeight - FP1.Z * POS_SCALE;
			pos2.y = windowHeight - FP2.Z * POS_SCALE;

			m_pRenderTarget->DrawLine(pos1, pos2, m_pLimitHeight, 1);
		}

		for (int idxPos = 1; idxPos < (int)RightFootVelList.size(); ++idxPos)
		{
			const FVector& FP1 = RightFootVelList[idxPos - 1];
			const FVector& FP2 = RightFootVelList[idxPos];

			D2D1_POINT_2F pos1;
			D2D1_POINT_2F pos2;

			pos1.x = float(idxPos - 1) * deltaX;
			pos2.x = float(idxPos) * deltaX;

			pos1.y = windowHeight - FP1.Size() * VEL_SCALE;
			pos2.y = windowHeight - FP2.Size() * VEL_SCALE;

			m_pRenderTarget->DrawLine(pos1, pos2, m_pLimitVelocity, 1);
		}
	}

	// draw left foot graph
	{
		float deltaX = float(windowWidth) / float(LeftFootPosList.size());

		for (int idxPos = 1; idxPos < (int)LeftFootPosList.size(); ++idxPos)
		{
			const FVector& FP1 = LeftFootPosList[idxPos - 1];
			const FVector& FP2 = LeftFootPosList[idxPos];

			D2D1_POINT_2F pos1;
			D2D1_POINT_2F pos2;

			pos1.x = float(idxPos - 1) * deltaX;
			pos2.x = float(idxPos) * deltaX;

			pos1.y = windowHeight - FP1.Z * POS_SCALE;
			pos2.y = windowHeight - FP2.Z * POS_SCALE;

			m_pRenderTarget->DrawLine(pos1, pos2, m_pLimitHeight, 1);
		}

		for (int idxPos = 1; idxPos < (int)LeftFootVelList.size(); ++idxPos)
		{
			const FVector& FP1 = LeftFootVelList[idxPos - 1];
			const FVector& FP2 = LeftFootVelList[idxPos];

			D2D1_POINT_2F pos1;
			D2D1_POINT_2F pos2;

			pos1.x = float(idxPos - 1) * deltaX;
			pos2.x = float(idxPos) * deltaX;

			pos1.y = windowHeight - FP1.Size() * VEL_SCALE;
			pos2.y = windowHeight - FP2.Size() * VEL_SCALE;

			m_pRenderTarget->DrawLine(pos1, pos2, m_pLimitVelocity, 1);
		}
	}
	

	// draw limit line
	/*{
		float limitVelY = ATTACK_MIN_HEIGHT * POS_SCALE;
		float limitPosY = windowHeight - limitVelY;

		D2D_POINT_2F pos1;
		pos1.x = 0.f;
		pos1.y = limitPosY;

		D2D_POINT_2F pos2;
		pos2.x = float(windowWidth);
		pos2.y = limitPosY;

		m_pRenderTarget->DrawLine(pos1, pos2, m_pLimitHeight, 2);

		limitVelY = PASS_MIN_VEL * VEL_SCALE;
		limitPosY = windowHeight - limitVelY;

		pos1.y = limitPosY;
		pos2.y = limitPosY;

		m_pRenderTarget->DrawLine(pos1, pos2, m_pLimitVelocity, 2);

		limitVelY = TOSS_MIN_VEL * VEL_SCALE;
		limitPosY = windowHeight - limitVelY;

		pos1.y = limitPosY;
		pos2.y = limitPosY;

		m_pRenderTarget->DrawLine(pos1, pos2, m_pLimitVelocity, 2);
	}*/


	bool bAttack = false;
	bool bPass = false;
	bool bToss = false;
	bool bUseLeftFoot;
	bool bDoMotion = false;

	// right foot
	if (bDoMotion == false && RightFootVelList.back().Size() * VEL_SCALE < 4.f)
	{
		bAttack = IsHigher(RightFootPosList, ATTACK_MIN_HEIGHT);
		bPass = IsFaster(RightFootVelList, PASS_MIN_VEL);
		bToss = IsFaster(RightFootVelList, TOSS_MIN_VEL);

		if (bAttack || bPass || bToss)
		{
			if (SearchMotionVector(RightFootPosList, RightFootVelList, RightFootPosList2))
			{
				RightFootVelList.clear();
				RightFootPosList.clear();
				RightFootPosList2.clear();

				bDoMotion = true;
				bUseLeftFoot = false;
			}
		}
	}


	// left foot
	if (bDoMotion == false && LeftFootVelList.back().Size() * VEL_SCALE < 4.f)
	{
		bAttack = IsHigher(LeftFootPosList, ATTACK_MIN_HEIGHT);
		bPass = IsFaster(LeftFootVelList, PASS_MIN_VEL);
		bToss = IsFaster(LeftFootVelList, TOSS_MIN_VEL);

		if (bAttack || bPass || bToss)
		{
			if (SearchMotionVector(LeftFootPosList, LeftFootVelList, LeftFootPosList2))
			{
				LeftFootVelList.clear();
				LeftFootPosList.clear();
				LeftFootPosList2.clear();

				bDoMotion = true;
				bUseLeftFoot = true;
			}
		}
	}
		
	if (bAttack)
	{
		SetStatusMessage(L"current motion: Attack");
	}
	else if (bPass)
	{
		SetStatusMessage(L"current motion: Pass");
	}
	else if (bToss)
	{
		SetStatusMessage(L"current motion: Toss");
	}
	else
	{
		SetStatusMessage(L"current motion: Idle");
	}

	m_pRenderTarget->DrawLine(MotionVectors2[0], MotionVectors2[1], m_pMotionVector, 3);
	m_pRenderTarget->DrawLine(MotionVectors2[1], MotionVectors2[2], m_pMotionVector, 3);
	m_pRenderTarget->DrawLine(MotionVectors2[2], MotionVectors2[3], m_pMotionVector, 3);
	m_pRenderTarget->DrawLine(MotionVectors2[3], MotionVectors2[4], m_pMotionVector, 3);


	if (bDoMotion)
	{
		SendInfo.bAttack = bAttack;
		SendInfo.bPass = bPass;
		SendInfo.bToss = bToss;
		SendInfo.bUseLeftFoot = bUseLeftFoot;

		for (int i = 0; i < 5; ++i)
		{
			SendInfo.MotionVectors[i] = MotionVectors[i];
		}

		send(clnt_sock, (char*)&SendInfo, sizeof(SendInfo), 0);

		bDoMotion = false;
	}


	// Render Torso
    DrawBone(skel, NUI_SKELETON_POSITION_HEAD, NUI_SKELETON_POSITION_SHOULDER_CENTER);
    DrawBone(skel, NUI_SKELETON_POSITION_SHOULDER_CENTER, NUI_SKELETON_POSITION_SHOULDER_LEFT);
    DrawBone(skel, NUI_SKELETON_POSITION_SHOULDER_CENTER, NUI_SKELETON_POSITION_SHOULDER_RIGHT);
    DrawBone(skel, NUI_SKELETON_POSITION_SHOULDER_CENTER, NUI_SKELETON_POSITION_SPINE);
    DrawBone(skel, NUI_SKELETON_POSITION_SPINE, NUI_SKELETON_POSITION_HIP_CENTER);
    DrawBone(skel, NUI_SKELETON_POSITION_HIP_CENTER, NUI_SKELETON_POSITION_HIP_LEFT);
    DrawBone(skel, NUI_SKELETON_POSITION_HIP_CENTER, NUI_SKELETON_POSITION_HIP_RIGHT);

    // Left Arm
    DrawBone(skel, NUI_SKELETON_POSITION_SHOULDER_LEFT, NUI_SKELETON_POSITION_ELBOW_LEFT);
    DrawBone(skel, NUI_SKELETON_POSITION_ELBOW_LEFT, NUI_SKELETON_POSITION_WRIST_LEFT);
    DrawBone(skel, NUI_SKELETON_POSITION_WRIST_LEFT, NUI_SKELETON_POSITION_HAND_LEFT);

    // Right Arm
    DrawBone(skel, NUI_SKELETON_POSITION_SHOULDER_RIGHT, NUI_SKELETON_POSITION_ELBOW_RIGHT);
    DrawBone(skel, NUI_SKELETON_POSITION_ELBOW_RIGHT, NUI_SKELETON_POSITION_WRIST_RIGHT);
    DrawBone(skel, NUI_SKELETON_POSITION_WRIST_RIGHT, NUI_SKELETON_POSITION_HAND_RIGHT);

    // Left Leg
    DrawBone(skel, NUI_SKELETON_POSITION_HIP_LEFT, NUI_SKELETON_POSITION_KNEE_LEFT);
    DrawBone(skel, NUI_SKELETON_POSITION_KNEE_LEFT, NUI_SKELETON_POSITION_ANKLE_LEFT);
    DrawBone(skel, NUI_SKELETON_POSITION_ANKLE_LEFT, NUI_SKELETON_POSITION_FOOT_LEFT);

    // Right Leg
    DrawBone(skel, NUI_SKELETON_POSITION_HIP_RIGHT, NUI_SKELETON_POSITION_KNEE_RIGHT);
    DrawBone(skel, NUI_SKELETON_POSITION_KNEE_RIGHT, NUI_SKELETON_POSITION_ANKLE_RIGHT);
    DrawBone(skel, NUI_SKELETON_POSITION_ANKLE_RIGHT, NUI_SKELETON_POSITION_FOOT_RIGHT);

    // Draw the joints in a different color
    for (i = 0; i < NUI_SKELETON_POSITION_COUNT; ++i)
    {
        D2D1_ELLIPSE ellipse = D2D1::Ellipse( m_Points[i], g_JointThickness, g_JointThickness );

        if ( skel.eSkeletonPositionTrackingState[i] == NUI_SKELETON_POSITION_INFERRED )
        {
            m_pRenderTarget->DrawEllipse(ellipse, m_pBrushJointInferred);
        }
        else if ( skel.eSkeletonPositionTrackingState[i] == NUI_SKELETON_POSITION_TRACKED )
        {
            m_pRenderTarget->DrawEllipse(ellipse, m_pBrushJointTracked);
        }
    }
}

/// <summary>
/// Draws a bone line between two joints
/// </summary>
/// <param name="skel">skeleton to draw bones from</param>
/// <param name="joint0">joint to start drawing from</param>
/// <param name="joint1">joint to end drawing at</param>
void CSkeletonBasics::DrawBone(const NUI_SKELETON_DATA & skel, NUI_SKELETON_POSITION_INDEX joint0, NUI_SKELETON_POSITION_INDEX joint1)
{
    NUI_SKELETON_POSITION_TRACKING_STATE joint0State = skel.eSkeletonPositionTrackingState[joint0];
    NUI_SKELETON_POSITION_TRACKING_STATE joint1State = skel.eSkeletonPositionTrackingState[joint1];

    // If we can't find either of these joints, exit
    if (joint0State == NUI_SKELETON_POSITION_NOT_TRACKED || joint1State == NUI_SKELETON_POSITION_NOT_TRACKED)
    {
        return;
    }

    // Don't draw if both points are inferred
    if (joint0State == NUI_SKELETON_POSITION_INFERRED && joint1State == NUI_SKELETON_POSITION_INFERRED)
    {
        return;
    }

    // We assume all drawn bones are inferred unless BOTH joints are tracked
    if (joint0State == NUI_SKELETON_POSITION_TRACKED && joint1State == NUI_SKELETON_POSITION_TRACKED)
    {
        m_pRenderTarget->DrawLine(m_Points[joint0], m_Points[joint1], m_pBrushBoneTracked, g_TrackedBoneThickness);
    }
    else
    {
        m_pRenderTarget->DrawLine(m_Points[joint0], m_Points[joint1], m_pBrushBoneInferred, g_InferredBoneThickness);
    }
}

/// <summary>
/// Converts a skeleton point to screen space
/// </summary>
/// <param name="skeletonPoint">skeleton point to tranform</param>
/// <param name="width">width (in pixels) of output buffer</param>
/// <param name="height">height (in pixels) of output buffer</param>
/// <returns>point in screen-space</returns>
D2D1_POINT_2F CSkeletonBasics::SkeletonToScreen(Vector4 skeletonPoint, int width, int height)
{
    LONG x, y;
    USHORT depth;

    // Calculate the skeleton's position on the screen
    // NuiTransformSkeletonToDepthImage returns coordinates in NUI_IMAGE_RESOLUTION_320x240 space
    NuiTransformSkeletonToDepthImage(skeletonPoint, &x, &y, &depth);

    float screenPointX = static_cast<float>(x * width) / cScreenWidth;
    float screenPointY = static_cast<float>(y * height) / cScreenHeight;

    return D2D1::Point2F(screenPointX, screenPointY);
}

/// <summary>
/// Ensure necessary Direct2d resources are created
/// </summary>
/// <returns>S_OK if successful, otherwise an error code</returns>
HRESULT CSkeletonBasics::EnsureDirect2DResources()
{
    HRESULT hr = S_OK;

    // If there isn't currently a render target, we need to create one
    if (NULL == m_pRenderTarget)
    {
        RECT rc;
        GetWindowRect( GetDlgItem( m_hWnd, IDC_VIDEOVIEW ), &rc );  

        int width = rc.right - rc.left;
        int height = rc.bottom - rc.top;
        D2D1_SIZE_U size = D2D1::SizeU( width, height );
        D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties();
        rtProps.pixelFormat = D2D1::PixelFormat( DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE);
        rtProps.usage = D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE;

        // Create a Hwnd render target, in order to render to the window set in initialize
        hr = m_pD2DFactory->CreateHwndRenderTarget(
            rtProps,
            D2D1::HwndRenderTargetProperties(GetDlgItem( m_hWnd, IDC_VIDEOVIEW), size),
            &m_pRenderTarget
            );
        if ( FAILED(hr) )
        {
            SetStatusMessage(L"Couldn't create Direct2D render target!");
            return hr;
        }

        //light green
        m_pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.27f, 0.75f, 0.27f), &m_pBrushJointTracked);

        m_pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Yellow, 1.0f), &m_pBrushJointInferred);
        m_pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Green, 1.0f), &m_pBrushBoneTracked);
        m_pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Gray, 1.0f), &m_pBrushBoneInferred);

		m_pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Orange, 3.0f), &m_pLimitHeight);
		m_pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Cyan, 3.0f), &m_pLimitVelocity);
		m_pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White, 3.0f), &m_pMotionVector);
    }

    return hr;
}

/// <summary>
/// Dispose Direct2d resources 
/// </summary>
void CSkeletonBasics::DiscardDirect2DResources( )
{
    SafeRelease(m_pRenderTarget);

    SafeRelease(m_pBrushJointTracked);
    SafeRelease(m_pBrushJointInferred);
    SafeRelease(m_pBrushBoneTracked);
    SafeRelease(m_pBrushBoneInferred);
}

/// <summary>
/// Set the status bar message
/// </summary>
/// <param name="szMessage">message to display</param>
void CSkeletonBasics::SetStatusMessage(WCHAR * szMessage)
{
    SendDlgItemMessageW(m_hWnd, IDC_STATUS, WM_SETTEXT, 0, (LPARAM)szMessage);
}