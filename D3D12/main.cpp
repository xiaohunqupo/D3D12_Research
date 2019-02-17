#include "stdafx.h"
#include "Graphics.h"

const int gWindowWidth = 1240;
const int gWindowHeight = 720;

#ifdef PLATFORM_UWP

using namespace Windows::ApplicationModel;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::UI::Core;
using namespace Windows::UI::Input;
using namespace Windows::UI::ViewManagement;
using namespace Windows::System;
using namespace Windows::Foundation;
using namespace Windows::Graphics::Display;
using namespace DirectX;

ref class ViewProvider sealed : public IFrameworkView
{
public:
	ViewProvider() :
		m_Exiting(false),
		m_IsVisible(true),
		m_IsResizing(false),
		m_DPI(96.f),
		m_LogicalWidth(gWindowWidth),
		m_LogicalHeight(gWindowHeight),
		m_NativeOrientation(DisplayOrientations::None),
		m_CurrentOrientation(DisplayOrientations::None)
	{
	}

	// IFrameworkView methods
	virtual void Initialize(CoreApplicationView^ applicationView)
	{
		applicationView->Activated +=
			ref new TypedEventHandler<CoreApplicationView^, IActivatedEventArgs^>(this, &ViewProvider::OnActivated);

		CoreApplication::Suspending +=
			ref new EventHandler<SuspendingEventArgs^>(this, &ViewProvider::OnSuspending);

		CoreApplication::Resuming +=
			ref new EventHandler<Platform::Object^>(this, &ViewProvider::OnResuming);

		m_pGame = std::make_unique<Graphics>((UINT)m_LogicalWidth, (UINT)m_LogicalHeight);
	}

	virtual void Uninitialize()
	{
		m_pGame.reset();
	}

	virtual void SetWindow(CoreWindow^ window)
	{
		window->SizeChanged +=
			ref new TypedEventHandler<CoreWindow^, WindowSizeChangedEventArgs^>(this, &ViewProvider::OnWindowSizeChanged);

#if defined(NTDDI_WIN10_RS2) && (NTDDI_VERSION >= NTDDI_WIN10_RS2)
		try
		{
			window->ResizeStarted +=
				ref new TypedEventHandler<CoreWindow^, Object^>(this, &ViewProvider::OnResizeStarted);

			window->ResizeCompleted +=
				ref new TypedEventHandler<CoreWindow^, Object^>(this, &ViewProvider::OnResizeCompleted);
		}
		catch (...)
		{
			// Requires Windows 10 Creators Update (10.0.15063) or later
		}
#endif

		window->VisibilityChanged +=
			ref new TypedEventHandler<CoreWindow^, VisibilityChangedEventArgs^>(this, &ViewProvider::OnVisibilityChanged);

		window->Closed +=
			ref new TypedEventHandler<CoreWindow^, CoreWindowEventArgs^>(this, &ViewProvider::OnWindowClosed);

		auto dispatcher = CoreWindow::GetForCurrentThread()->Dispatcher;

		dispatcher->AcceleratorKeyActivated +=
			ref new TypedEventHandler<CoreDispatcher^, AcceleratorKeyEventArgs^>(this, &ViewProvider::OnAcceleratorKeyActivated);

		auto navigation = Windows::UI::Core::SystemNavigationManager::GetForCurrentView();

		navigation->BackRequested +=
			ref new EventHandler<BackRequestedEventArgs^>(this, &ViewProvider::OnBackRequested);

		auto currentDisplayInformation = DisplayInformation::GetForCurrentView();

		currentDisplayInformation->DpiChanged +=
			ref new TypedEventHandler<DisplayInformation^, Object^>(this, &ViewProvider::OnDpiChanged);

		currentDisplayInformation->OrientationChanged +=
			ref new TypedEventHandler<DisplayInformation^, Object^>(this, &ViewProvider::OnOrientationChanged);

		DisplayInformation::DisplayContentsInvalidated +=
			ref new TypedEventHandler<DisplayInformation^, Object^>(this, &ViewProvider::OnDisplayContentsInvalidated);

		m_DPI = currentDisplayInformation->LogicalDpi;

		m_LogicalWidth = window->Bounds.Width;
		m_LogicalHeight = window->Bounds.Height;

		m_NativeOrientation = currentDisplayInformation->NativeOrientation;
		m_CurrentOrientation = currentDisplayInformation->CurrentOrientation;

		int outputWidth = ConvertDipsToPixels(m_LogicalWidth);
		int outputHeight = ConvertDipsToPixels(m_LogicalHeight);

		DXGI_MODE_ROTATION rotation = ComputeDisplayRotation();

		if (rotation == DXGI_MODE_ROTATION_ROTATE90 || rotation == DXGI_MODE_ROTATION_ROTATE270)
		{
			std::swap(outputWidth, outputHeight);
		}
		m_pGame->Initialize(window);
	}

	virtual void Load(Platform::String^ entryPoint)
	{
	}

	virtual void Run()
	{
		GameTimer::Reset();
		while (!m_Exiting)
		{
			if (m_IsVisible)
			{
				GameTimer::Tick();
				m_pGame->Update();
				CoreWindow::GetForCurrentThread()->Dispatcher->ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
			}
			else
			{
				CoreWindow::GetForCurrentThread()->Dispatcher->ProcessEvents(CoreProcessEventsOption::ProcessOneAndAllPending);
			}
		}
	}

protected:
	// Event handlers
	void OnActivated(CoreApplicationView^ applicationView, IActivatedEventArgs^ args)
	{
		if (args->Kind == ActivationKind::Launch)
		{
			auto launchArgs = static_cast<LaunchActivatedEventArgs^>(args);

			if (launchArgs->PrelaunchActivated)
			{
				// Opt-out of Prelaunch
				CoreApplication::Exit();
				return;
			}
		}

		int w = 1240, h = 720;

		m_DPI = DisplayInformation::GetForCurrentView()->LogicalDpi;
		ApplicationView::PreferredLaunchWindowingMode = ApplicationViewWindowingMode::PreferredLaunchViewSize;
		auto desiredSize = Size(ConvertPixelsToDips(w), ConvertPixelsToDips(h));
		ApplicationView::PreferredLaunchViewSize = desiredSize;
		auto view = ApplicationView::GetForCurrentView();
		auto minSize = Size(ConvertPixelsToDips(320), ConvertPixelsToDips(200));
		view->SetPreferredMinSize(minSize);
		CoreWindow::GetForCurrentThread()->Activate();
		view->FullScreenSystemOverlayMode = FullScreenSystemOverlayMode::Minimal;
		view->TryResizeView(desiredSize);
	}

	void OnSuspending(Platform::Object^ sender, SuspendingEventArgs^ args)
	{
	}

	void OnResuming(Platform::Object^ sender, Platform::Object^ args)
	{
	}

	void OnWindowSizeChanged(CoreWindow^ sender, WindowSizeChangedEventArgs^ args)
	{
		m_LogicalWidth = sender->Bounds.Width;
		m_LogicalHeight = sender->Bounds.Height;

		if (m_IsResizing)
		{
			return;
		}
		HandleWindowSizeChanged();
	}

#if defined(NTDDI_WIN10_RS2) && (NTDDI_VERSION >= NTDDI_WIN10_RS2)
	void OnResizeStarted(CoreWindow^ sender, Platform::Object^ args)
	{
		m_IsResizing = true;
	}

	void OnResizeCompleted(CoreWindow^ sender, Platform::Object^ args)
	{
		m_IsResizing = false;
		HandleWindowSizeChanged();
	}
#endif

	void OnVisibilityChanged(CoreWindow^ sender, VisibilityChangedEventArgs^ args)
	{

	}

	void OnWindowClosed(CoreWindow^ sender, CoreWindowEventArgs^ args)
	{
		m_Exiting = true;
	}

	void OnAcceleratorKeyActivated(CoreDispatcher^, AcceleratorKeyEventArgs^ args)
	{
		if (args->EventType == CoreAcceleratorKeyEventType::SystemKeyDown
			&& args->VirtualKey == VirtualKey::Enter
			&& args->KeyStatus.IsMenuKeyDown
			&& !args->KeyStatus.WasKeyDown)
		{
			// Implements the classic ALT+ENTER fullscreen toggle
			auto view = ApplicationView::GetForCurrentView();

			if (view->IsFullScreenMode)
				view->ExitFullScreenMode();
			else
				view->TryEnterFullScreenMode();

			args->Handled = true;
		}
	}

	void OnBackRequested(Platform::Object^, Windows::UI::Core::BackRequestedEventArgs^ args)
	{
		// UWP on Xbox One triggers a back request whenever the B button is pressed
		// which can result in the app being suspended if unhandled
		args->Handled = true;
	}

	void OnDpiChanged(DisplayInformation^ sender, Object^ args)
	{
		m_DPI = sender->LogicalDpi;

		HandleWindowSizeChanged();
	}

	void OnOrientationChanged(DisplayInformation^ sender, Object^ args)
	{
		auto resizeManager = CoreWindowResizeManager::GetForCurrentView();
		resizeManager->ShouldWaitForLayoutCompletion = true;

		m_CurrentOrientation = sender->CurrentOrientation;

		HandleWindowSizeChanged();

		resizeManager->NotifyLayoutCompleted();
	}

	void OnDisplayContentsInvalidated(DisplayInformation^ sender, Object^ args)
	{
	}

private:
	inline int ConvertDipsToPixels(float dips) const
	{
		return int(dips * m_DPI / 96.f + 0.5f);
	}

	inline float ConvertPixelsToDips(int pixels) const
	{
		return (float(pixels) * 96.f / m_DPI);
	}

	DXGI_MODE_ROTATION ComputeDisplayRotation() const
	{
		DXGI_MODE_ROTATION rotation = DXGI_MODE_ROTATION_UNSPECIFIED;

		switch (m_NativeOrientation)
		{
		case DisplayOrientations::Landscape:
			switch (m_CurrentOrientation)
			{
			case DisplayOrientations::Landscape:
				rotation = DXGI_MODE_ROTATION_IDENTITY;
				break;

			case DisplayOrientations::Portrait:
				rotation = DXGI_MODE_ROTATION_ROTATE270;
				break;

			case DisplayOrientations::LandscapeFlipped:
				rotation = DXGI_MODE_ROTATION_ROTATE180;
				break;

			case DisplayOrientations::PortraitFlipped:
				rotation = DXGI_MODE_ROTATION_ROTATE90;
				break;
			}
			break;

		case DisplayOrientations::Portrait:
			switch (m_CurrentOrientation)
			{
			case DisplayOrientations::Landscape:
				rotation = DXGI_MODE_ROTATION_ROTATE90;
				break;

			case DisplayOrientations::Portrait:
				rotation = DXGI_MODE_ROTATION_IDENTITY;
				break;

			case DisplayOrientations::LandscapeFlipped:
				rotation = DXGI_MODE_ROTATION_ROTATE270;
				break;

			case DisplayOrientations::PortraitFlipped:
				rotation = DXGI_MODE_ROTATION_ROTATE180;
				break;
			}
			break;
		}

		return rotation;
	}

	void HandleWindowSizeChanged()
	{
		int outputWidth = ConvertDipsToPixels(m_LogicalWidth);
		int outputHeight = ConvertDipsToPixels(m_LogicalHeight);

		DXGI_MODE_ROTATION rotation = ComputeDisplayRotation();

		if (rotation == DXGI_MODE_ROTATION_ROTATE90 || rotation == DXGI_MODE_ROTATION_ROTATE270)
		{
			std::swap(outputWidth, outputHeight);
		}
		m_pGame->OnResize(outputWidth, outputHeight);
	}
	bool m_Exiting;
	bool m_IsVisible;
	bool m_IsResizing;
	float m_DPI;
	float m_LogicalWidth;
	float m_LogicalHeight;
	std::unique_ptr<Graphics> m_pGame;

	Windows::Graphics::Display::DisplayOrientations	m_NativeOrientation;
	Windows::Graphics::Display::DisplayOrientations	m_CurrentOrientation;
};

ref class ViewProviderFactory : IFrameworkViewSource
{
public:
	virtual IFrameworkView^ CreateView()
	{
		return ref new ViewProvider();
	}
};

// Entry point
[Platform::MTAThread]
int __cdecl main(Platform::Array<Platform::String^>^ /*argv*/)
{
	if (!XMVerifyCPUSupport())
	{
		throw std::exception("XMVerifyCPUSupport");
	}

	auto viewProviderFactory = ref new ViewProviderFactory();
	CoreApplication::Run(viewProviderFactory);
	return 0;
}

#else

class ViewWrapper
{
public:
	void Run(const char* pTitle)
	{
		MakeWindow(pTitle);

		m_pGraphics = std::make_unique<Graphics>(gWindowWidth, gWindowHeight);
		m_pGraphics->Initialize(m_Window);

		GameTimer::Reset();
		//Game loop
		MSG msg = {};
		while (msg.message != WM_QUIT)
		{
			GameTimer::Tick();
			if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
			{
				TranslateMessage(&msg);
				DispatchMessage(&msg);
			}
			else
			{
				m_pGraphics->Update();
			}
		}
		m_pGraphics->Shutdown();
	}

private:
	static LRESULT CALLBACK WndProcStatic(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
	{
		ViewWrapper* pThis = nullptr;

		if (message == WM_NCCREATE)
		{
			pThis = static_cast<ViewWrapper*>(reinterpret_cast<CREATESTRUCT*>(lParam)->lpCreateParams);
			SetLastError(0);
			if (!SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis)))
			{
				if (GetLastError() != 0)
					return 0;
			}
		}
		else
		{
			pThis = reinterpret_cast<ViewWrapper*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));
		}
		if (pThis)
		{
			LRESULT callback = pThis->WndProc(hWnd, message, wParam, lParam);
			return callback;
		}
		return DefWindowProc(hWnd, message, wParam, lParam);
	}

	LRESULT WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
	{
		switch (message)
		{
			// WM_SIZE is sent when the user resizes the window.  
		case WM_SIZE:
		{			
			// Save the new client area dimensions.
			int windowWidth = LOWORD(lParam);
			int windowHeight = HIWORD(lParam);
			if (m_pGraphics && windowWidth > 0 && windowHeight > 0)
			{
				m_pGraphics->OnResize(windowWidth, windowHeight);
			}
			return 0;
		}
		case WM_KEYUP:
		{
			if (wParam == VK_ESCAPE)
			{
				PostQuitMessage(0);
			}
			return 0;
		}
		case WM_DESTROY:
		{
			PostQuitMessage(0);
			return 0;
		}
		}

		return DefWindowProc(hWnd, message, wParam, lParam);
	}
	
	void MakeWindow(const char* pTitle)
	{
		WNDCLASS wc;

		wc.hInstance = GetModuleHandle(0);
		wc.cbClsExtra = 0;
		wc.cbWndExtra = 0;
		wc.hIcon = 0;
		wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
		wc.lpfnWndProc = WndProcStatic;
		wc.style = CS_HREDRAW | CS_VREDRAW;
		wc.lpszClassName = "wndClass";
		wc.lpszMenuName = nullptr;
		wc.hCursor = LoadCursor(nullptr, IDC_ARROW);

		if (!RegisterClass(&wc))
		{
			return;
		}

		int displayWidth = GetSystemMetrics(SM_CXSCREEN);
		int displayHeight = GetSystemMetrics(SM_CYSCREEN);

		DWORD windowStyle = WS_OVERLAPPEDWINDOW;

		RECT windowRect = { 0, 0, (LONG)gWindowWidth, (LONG)gWindowHeight };
		AdjustWindowRect(&windowRect, windowStyle, false);

		int x = (displayWidth - gWindowWidth) / 2;
		int y = (displayHeight - gWindowHeight) / 2;

		m_Window = CreateWindow(
			"wndClass",
			pTitle,
			windowStyle,
			x,
			y,
			gWindowWidth,
			gWindowHeight,
			nullptr,
			nullptr,
			GetModuleHandle(0),
			this
		);

		if (m_Window == nullptr)
			return;

		ShowWindow(m_Window, SW_SHOWDEFAULT);
		if (!UpdateWindow(m_Window))
		{
			return;
		}
	}

private:
	HWND m_Window;
	std::unique_ptr<Graphics> m_pGraphics;
};

int main()
{
	ViewWrapper vp;
	vp.Run("D3D12 - Hello World");
}

#endif