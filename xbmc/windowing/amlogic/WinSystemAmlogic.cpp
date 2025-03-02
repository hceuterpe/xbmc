/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "WinSystemAmlogic.h"

#include <string.h>
#include <float.h>

#include "ServiceBroker.h"
#include "cores/RetroPlayer/process/amlogic/RPProcessInfoAmlogic.h"
#include "cores/RetroPlayer/rendering/VideoRenderers/RPRendererOpenGLES.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodecAmlogic.h"
#include "cores/VideoPlayer/VideoRenderers/LinuxRendererGLES.h"
#include "cores/VideoPlayer/VideoRenderers/HwDecRender/RendererAML.h"
#include "windowing/GraphicContext.h"
#include "windowing/Resolution.h"
#include "platform/linux/powermanagement/LinuxPowerSyscall.h"
#include "platform/linux/FDEventMonitor.h"
#include "platform/linux/ScreenshotSurfaceAML.h"
#include "settings/DisplaySettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "settings/lib/Setting.h"
#include "guilib/DispResource.h"
#include "utils/AMLUtils.h"
#include "utils/BitstreamConverter.h"
#include "utils/log.h"
#include "threads/SingleLock.h"

#include "platform/linux/SysfsPath.h"

#include <linux/fb.h>
#include <linux/version.h>
#include <poll.h>
#include <unistd.h>

#include "system_egl.h"

using namespace KODI;

CWinSystemAmlogic::CWinSystemAmlogic()
:  m_nativeWindow(NULL)
,  m_libinput(new CLibInputHandler)
,  m_force_mode_switch(false)
,  m_fdMonitorId(0)
,  m_udev(NULL)
,  m_udevMonitor(NULL)
{
  const char *env_framebuffer = getenv("FRAMEBUFFER");

  // default to framebuffer 0
  m_framebuffer_name = "fb0";
  if (env_framebuffer)
  {
    std::string framebuffer(env_framebuffer);
    std::string::size_type start = framebuffer.find("fb");
    m_framebuffer_name = framebuffer.substr(start);
  }

  m_nativeDisplay = EGL_NO_DISPLAY;

  m_stereo_mode = RENDER_STEREO_MODE_OFF;
  m_delayDispReset = false;

  m_libinput->Start();
}

CWinSystemAmlogic::~CWinSystemAmlogic()
{
  MonitorStop();
}

void CWinSystemAmlogic::MonitorStart()
{
  int err;

  if (!m_udev)
  {
    m_udev = udev_new();
    if (!m_udev)
    {
      CLog::Log(LOGWARNING, "CWinSystemAmlogic::Start - Unable to open udev handle");
      return;
    }

    m_udevMonitor = udev_monitor_new_from_netlink(m_udev, "udev");
    if (!m_udevMonitor)
    {
      CLog::Log(LOGERROR, "CWinSystemAmlogic::Start - udev_monitor_new_from_netlink() failed");
      goto err_unref_udev;
    }

    err = udev_monitor_filter_add_match_subsystem_devtype(m_udevMonitor, "drm", NULL);
    if (err)
    {
      CLog::Log(LOGERROR, "CWinSystemAmlogic::Start - udev_monitor_filter_add_match_subsystem_devtype() failed");
      goto err_unref_monitor;
    }

    err = udev_monitor_enable_receiving(m_udevMonitor);
    if (err)
    {
      CLog::Log(LOGERROR, "CWinSystemAmlogic::Start - udev_monitor_enable_receiving() failed");
      goto err_unref_monitor;
    }

    const auto eventMonitor = CServiceBroker::GetPlatform().GetService<CFDEventMonitor>();
    eventMonitor->AddFD(
        CFDEventMonitor::MonitoredFD(udev_monitor_get_fd(m_udevMonitor),
                                     POLLIN, FDEventCallback, m_udevMonitor),
        m_fdMonitorId);
  }

  return;

err_unref_monitor:
  udev_monitor_unref(m_udevMonitor);
  m_udevMonitor = NULL;
err_unref_udev:
  udev_unref(m_udev);
  m_udev = NULL;
}

void CWinSystemAmlogic::MonitorStop()
{
  if (m_udev)
  {
    const auto eventMonitor = CServiceBroker::GetPlatform().GetService<CFDEventMonitor>();
    eventMonitor->RemoveFD(m_fdMonitorId);

    udev_monitor_unref(m_udevMonitor);
    m_udevMonitor = NULL;
    udev_unref(m_udev);
    m_udev = NULL;
  }
}


void CWinSystemAmlogic::HotplugEvent()
{
  std::string preferred_mode = aml_get_preferred_mode();
  CLog::Log(LOGDEBUG, "CWinSystemAmlogic - HotplugEvent, preferred mode: {}", preferred_mode);

  if (!preferred_mode.empty())
  {
    aml_set_hotplug_mode(preferred_mode);

    // clear screen by fb blank
    usleep(500 * 1000);
    CSysfsPath("/sys/class/graphics/fb0/blank", 1);
    usleep(500 * 1000);
    CSysfsPath("/sys/class/graphics/fb0/blank", 0);
  }
}

void CWinSystemAmlogic::FDEventCallback(int id, int fd, short revents, void *data)
{
  struct udev_monitor *udevMonitor = (struct udev_monitor *)data;
  struct udev_device *device;

  while ((device = udev_monitor_receive_device(udevMonitor)) != NULL)
  {
    const char* action = udev_device_get_action(device);
    CLog::Log(LOGDEBUG, "CWinSystemAmlogic - FDEventCallback (\"{}\", \"{}\"), action: {}",
      udev_device_get_syspath(device), udev_device_get_devpath(device), action);

    if (StringUtils::EqualsNoCase(action, "change"))
      HotplugEvent();
  }
}

bool CWinSystemAmlogic::InitWindowSystem()
{
  const std::shared_ptr<CSettings> settings = CServiceBroker::GetSettingsComponent()->GetSettings();

  if (settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_NOISEREDUCTION))
  {
     CLog::Log(LOGDEBUG, "CWinSystemAmlogic::InitWindowSystem -- disabling noise reduction");
     CSysfsPath("/sys/module/aml_media/parameters/nr2_en", 0);
  }

  int sdr2hdr = settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_SDR2HDR);
  if (sdr2hdr)
  {
    CLog::Log(LOGDEBUG, "CWinSystemAmlogic::InitWindowSystem -- setting sdr2hdr mode to {:d}", sdr2hdr);
    CSysfsPath("/sys/module/aml_media/parameters/sdr_mode", 1);
    CSysfsPath("/sys/module/aml_media/parameters/dolby_vision_policy", 0);
    CSysfsPath("/sys/module/aml_media/parameters/hdr_policy", 0);
  }

  int hdr2sdr = settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_HDR2SDR);
  if (hdr2sdr)
  {
    CLog::Log(LOGDEBUG, "CWinSystemAmlogic::InitWindowSystem -- setting hdr2sdr mode to {:d}", hdr2sdr);
    CSysfsPath("/sys/module/aml_media/parameters/hdr_mode", 1);
  }

  if (!aml_support_dolby_vision() || !aml_display_support_dv())
  {
    auto setting = settings->GetSetting(CSettings::SETTING_COREELEC_AMLOGIC_DV_DISABLE);
    if (setting)
    {
      setting->SetVisible(false);
      settings->SetBool(CSettings::SETTING_COREELEC_AMLOGIC_DV_DISABLE, false);
    }

    setting = settings->GetSetting(CSettings::SETTING_COREELEC_AMLOGIC_USE_PLAYERLED);
    if (setting)
    {
      setting->SetVisible(false);
      settings->SetBool(CSettings::SETTING_COREELEC_AMLOGIC_USE_PLAYERLED, false);
    }
  }
  else
  {
    int dv_cap = aml_get_drmProperty("dv_cap", DRM_MODE_OBJECT_CONNECTOR);
    CLog::Log(LOGDEBUG, "CWinSystemAmlogic::InitWindowSystem -- got display dv_cap: {:d}", dv_cap);
    if (dv_cap != -1 && ((dv_cap & LL_YCbCr_422_12BIT) != 0))
    {
      auto setting = settings->GetSetting(CSettings::SETTING_COREELEC_AMLOGIC_USE_PLAYERLED);
      if (setting)
        setting->SetVisible(true);
    }
  }

  if (((LINUX_VERSION_CODE >> 16) & 0xFF) < 5)
  {
    auto setting = settings->GetSetting(CSettings::SETTING_COREELEC_AMLOGIC_DISABLEGUISCALING);
    if (setting)
    {
      setting->SetVisible(false);
      settings->SetBool(CSettings::SETTING_COREELEC_AMLOGIC_DISABLEGUISCALING, false);
    }
  }

  m_nativeDisplay = EGL_DEFAULT_DISPLAY;

  CDVDVideoCodecAmlogic::Register();
  CLinuxRendererGLES::Register();
  RETRO::CRPProcessInfoAmlogic::Register();
  RETRO::CRPProcessInfoAmlogic::RegisterRendererFactory(new RETRO::CRendererFactoryOpenGLES);
  CRendererAML::Register();
  CScreenshotSurfaceAML::Register();

  if (aml_get_cpufamily_id() <= AML_GXL)
    aml_set_framebuffer_resolution(1920, 1080, m_framebuffer_name);

  auto setting = settings->GetSetting(CSettings::SETTING_VIDEOPLAYER_USEDISPLAYASCLOCK);
  if (setting)
  {
    setting->SetVisible(false);
    settings->SetBool(CSettings::SETTING_VIDEOPLAYER_USEDISPLAYASCLOCK, false);
  }

  // Close the OpenVFD splash and switch the display into time mode.
  CSysfsPath("/tmp/openvfd_service", 0);

  drmModeConnection connection;
  int mode_count = aml_get_drmDevice_modes_count(&connection);

  if (connection == DRM_MODE_DISCONNECTED)
  {
    if (mode_count > 1)
    {
      CLog::Log(LOGDEBUG, "CWinSystemAmlogic::InitWindowSystem Looks like display was hotplugged before Kodi start");
      HotplugEvent();
    }
    else if (mode_count == 1)
    {
      CLog::Log(LOGDEBUG, "CWinSystemAmlogic::InitWindowSystem Looks like no display is connected, wait for hotplug");
      MonitorStart();
    }
  }

  // kill a running animation
  CLog::Log(LOGDEBUG,"CWinSystemAmlogic: Sending SIGUSR1 to 'splash-image'");
  std::system("killall -s SIGUSR1 splash-image &> /dev/null");

  return CWinSystemBase::InitWindowSystem();
}

bool CWinSystemAmlogic::DestroyWindowSystem()
{
  return true;
}

bool CWinSystemAmlogic::CreateNewWindow(const std::string& name,
                                    bool fullScreen,
                                    RESOLUTION_INFO& res)
{
  m_nWidth        = res.iWidth;
  m_nHeight       = res.iHeight;
  m_fRefreshRate  = res.fRefreshRate;

  if (m_nativeWindow == NULL)
    m_nativeWindow = new fbdev_window;

  m_nativeWindow->width = res.iWidth;
  m_nativeWindow->height = res.iHeight;

  int delay = CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt("videoscreen.delayrefreshchange");
  if (delay > 0)
  {
    m_delayDispReset = true;
    m_dispResetTimer.Set(std::chrono::milliseconds(static_cast<unsigned int>(delay * 100)));
  }

  {
    std::unique_lock<CCriticalSection> lock(m_resourceSection);
    for (std::vector<IDispResource *>::iterator i = m_resources.begin(); i != m_resources.end(); ++i)
    {
      (*i)->OnLostDisplay();
    }
  }

  aml_set_native_resolution(res, m_framebuffer_name, m_stereo_mode, m_force_mode_switch);
  // reset force mode switch
  m_force_mode_switch = false;

  if (!m_delayDispReset)
  {
    std::unique_lock<CCriticalSection> lock(m_resourceSection);
    // tell any shared resources
    for (std::vector<IDispResource *>::iterator i = m_resources.begin(); i != m_resources.end(); ++i)
    {
      (*i)->OnResetDisplay();
    }
  }

  m_bWindowCreated = true;
  return true;
}

bool CWinSystemAmlogic::DestroyWindow()
{
  if (m_nativeWindow != NULL)
  {
    delete(m_nativeWindow);
    m_nativeWindow = NULL;
  }

  m_bWindowCreated = false;
  return true;
}

void CWinSystemAmlogic::UpdateResolutions()
{
  CWinSystemBase::UpdateResolutions();

  CDisplaySettings::GetInstance().ClearCustomResolutions();

  RESOLUTION_INFO resDesktop, curDisplay;
  std::vector<RESOLUTION_INFO> resolutions;

  if (!aml_probe_resolutions(resolutions) || resolutions.empty())
    CLog::Log(LOGWARNING, "{}: ProbeResolutions failed.",__FUNCTION__);

  // get all resolutions supported by connected device
  if (aml_get_native_resolution(&curDisplay))
    resDesktop = curDisplay;

  for (auto& res : resolutions)
  {
    CLog::Log(LOGINFO, "Found resolution {:d} x {:d} with {:d} x {:d}{} @ {:f} Hz",
      res.iWidth,
      res.iHeight,
      res.iScreenWidth,
      res.iScreenHeight,
      res.dwFlags & D3DPRESENTFLAG_INTERLACED ? "i" : "",
      res.fRefreshRate);

    // add new custom resolution
    CServiceBroker::GetWinSystem()->GetGfxContext().ResetOverscan(res);
    CDisplaySettings::GetInstance().AddResolutionInfo(res);

    // check if resolution match current mode
    if(resDesktop.iWidth == res.iWidth &&
       resDesktop.iHeight == res.iHeight &&
       resDesktop.iScreenWidth == res.iScreenWidth &&
       resDesktop.iScreenHeight == res.iScreenHeight &&
       (resDesktop.dwFlags & D3DPRESENTFLAG_MODEMASK) == (res.dwFlags & D3DPRESENTFLAG_MODEMASK) &&
       fabs(resDesktop.fRefreshRate - res.fRefreshRate) < FLT_EPSILON)
    {
      // update desktop resolution
      CDisplaySettings::GetInstance().GetResolutionInfo(RES_DESKTOP) = res;
    }
  }
}

bool CWinSystemAmlogic::IsHDRDisplay()
{
  CSysfsPath hdr_cap{"/sys/class/amhdmitx/amhdmitx0/hdr_cap"};
  CSysfsPath dv_cap{"/sys/class/amhdmitx/amhdmitx0/dv_cap"};
  std::string valstr;

  if (hdr_cap.Exists())
  {
    valstr = hdr_cap.Get<std::string>().value();
    if (valstr.find("Traditional HDR: 1") != std::string::npos)
      m_hdr_caps.SetHDR10();

    if (valstr.find("HDR10Plus Supported: 1") != std::string::npos)
      m_hdr_caps.SetHDR10Plus();

    if (valstr.find("Hybrid Log-Gamma: 1") != std::string::npos)
      m_hdr_caps.SetHLG();
  }

  if (dv_cap.Exists())
  {
    valstr = dv_cap.Get<std::string>().value();
    if (valstr.find("DolbyVision RX support list") != std::string::npos)
      m_hdr_caps.SetDolbyVision();
  }

  return (m_hdr_caps.SupportsHDR10() | m_hdr_caps.SupportsHDR10Plus() | m_hdr_caps.SupportsHLG());
}

CHDRCapabilities CWinSystemAmlogic::GetDisplayHDRCapabilities() const
{
  return m_hdr_caps;
}

float CWinSystemAmlogic::GetGuiSdrPeakLuminance() const
{
  const auto settings = CServiceBroker::GetSettingsComponent()->GetSettings();
  const int guiSdrPeak = settings->GetInt(CSettings::SETTING_VIDEOSCREEN_GUISDRPEAKLUMINANCE);

  return ((0.7f * guiSdrPeak + 30.0f) / 100.0f);
}

bool CWinSystemAmlogic::Hide()
{
  return false;
}

bool CWinSystemAmlogic::Show(bool show)
{
  CSysfsPath("/sys/class/graphics/" + m_framebuffer_name + "/blank", (show ? 0 : 1));
  return true;
}

void CWinSystemAmlogic::Register(IDispResource *resource)
{
  std::unique_lock<CCriticalSection> lock(m_resourceSection);
  m_resources.push_back(resource);
}

void CWinSystemAmlogic::Unregister(IDispResource *resource)
{
  std::unique_lock<CCriticalSection> lock(m_resourceSection);
  std::vector<IDispResource*>::iterator i = find(m_resources.begin(), m_resources.end(), resource);
  if (i != m_resources.end())
    m_resources.erase(i);
}
