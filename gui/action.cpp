/*
	Copyright 2013 bigbiff/Dees_Troy TeamWin
	This file is part of TWRP/TeamWin Recovery Project.

	Copyright (C) 2018-2021 OrangeFox Recovery Project
	This file is part of the OrangeFox Recovery Project.

	TWRP is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	TWRP is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with TWRP.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <dirent.h>
#include <private/android_filesystem_config.h>
#include <android-base/properties.h>

#include <string>
#include <sstream>
#include "../partitions.hpp"
#include "../twrp-functions.hpp"
#include "../twrpRepacker.hpp"
#include "../openrecoveryscript.hpp"

#include "twinstall/adb_install.h"

#include "fuse_sideload.h"
#include "blanktimer.hpp"
#include "twinstall.h"
#include "../twrpDigest/twrpSHA.hpp"
#include "../twrpDigestDriver.hpp"

#include "../orangefox.hpp"

extern "C" {
#include "../twcommon.h"
#include "../variables.h"
#include "cutils/properties.h"
#include "install/adb_install.h"
};
#include "set_metadata.h"
#include "../minuitwrp/minui.h"

#include "rapidxml.hpp"
#include "objects.hpp"
#include "tw_atomic.hpp"

GUIAction::mapFunc GUIAction::mf;
std::set < string > GUIAction::setActionsRunningInCallerThread;
static string zip_queue[10];
static int zip_queue_index;
pid_t sideload_child_pid;

static void *ActionThread_work_wrapper(void *data);

class ActionThread
{
public:
  ActionThread();
  ~ActionThread();

  void threadActions(GUIAction * act);
  void run(void *data);
private:
  friend void *ActionThread_work_wrapper(void *);
  struct ThreadData
  {
    ActionThread *this_;
    GUIAction *act;
    ThreadData(ActionThread * this_, GUIAction * act):this_(this_), act(act)
    {
    }
  };

  pthread_t m_thread;
  bool m_thread_running;
  pthread_mutex_t m_act_lock;
};

static ActionThread action_thread;	// for all kinds of longer running actions
static ActionThread cancel_thread;	// for longer running "cancel" actions

static void *ActionThread_work_wrapper(void *data)
{
  static_cast < ActionThread::ThreadData * >(data)->this_->run(data);
  return NULL;
}

ActionThread::ActionThread()
{
  m_thread_running = false;
  pthread_mutex_init(&m_act_lock, NULL);
}

ActionThread::~ActionThread()
{
  pthread_mutex_lock(&m_act_lock);
  if (m_thread_running)
    {
      pthread_mutex_unlock(&m_act_lock);
      pthread_join(m_thread, NULL);
    }
  else
    {
      pthread_mutex_unlock(&m_act_lock);
    }
  pthread_mutex_destroy(&m_act_lock);
}

// custom encryption kludge - DJ9
static bool Hide_Reboot_Kludge_Fix(string FuncName)
{
  return (FuncName == "need_reboot");
}
// kludge - DJ9

void ActionThread::threadActions(GUIAction * act)
{
  pthread_mutex_lock(&m_act_lock);
  if (m_thread_running)
    {
      pthread_mutex_unlock(&m_act_lock);
     if (! Hide_Reboot_Kludge_Fix(act->mActions[0].mFunction))
      LOGERR
	("Another threaded action is already running -- not running %u actions starting with '%s'\n",
	 act->mActions.size(), act->mActions[0].mFunction.c_str());
    }
  else
    {
      m_thread_running = true;
      pthread_mutex_unlock(&m_act_lock);
      ThreadData *d = new ThreadData(this, act);
      pthread_create(&m_thread, NULL, &ActionThread_work_wrapper, d);
    }
}

void ActionThread::run(void *data)
{
  ThreadData *d = (ThreadData *) data;
  GUIAction *act = d->act;

  std::vector < GUIAction::Action >::iterator it;
  for (it = act->mActions.begin(); it != act->mActions.end(); ++it)
    act->doAction(*it);

  pthread_mutex_lock(&m_act_lock);
  m_thread_running = false;
  pthread_mutex_unlock(&m_act_lock);
  delete d;
}

GUIAction::GUIAction(xml_node <> *node):GUIObject(node)
{
  xml_node <> *child;
  xml_node <> *actions;
  xml_attribute <> *attr;

  if (!node)
    return;

  if (mf.empty())
    {
#define ADD_ACTION(n) mf[#n] = &GUIAction::n
#define ADD_ACTION_EX(name, func) mf[name] = &GUIAction::func
      // These actions will be run in the caller's thread
      ADD_ACTION(reboot);
      ADD_ACTION(home);
      ADD_ACTION(key);
      ADD_ACTION(page);
      ADD_ACTION(reload);
      ADD_ACTION(check_and_reload);
      ADD_ACTION(readBackup);
      ADD_ACTION(set);
      ADD_ACTION(clear);
      ADD_ACTION(mount);
      ADD_ACTION(unmount);
      ADD_ACTION_EX("umount", unmount);
      ADD_ACTION(restoredefaultsettings);
      ADD_ACTION(copylog);
      ADD_ACTION(compute);
      ADD_ACTION_EX("addsubtract", compute);
      ADD_ACTION(setguitimezone);
      ADD_ACTION(overlay);
      ADD_ACTION(queuezip);
      ADD_ACTION(cancelzip);
      ADD_ACTION(queueclear);
      ADD_ACTION(sleep);
      ADD_ACTION(sleepcounter);
      ADD_ACTION(appenddatetobackupname);
      ADD_ACTION(generatebackupname);
      ADD_ACTION(checkpartitionlist);
      ADD_ACTION(getpartitiondetails);
      ADD_ACTION(screenshot);
      ADD_ACTION_EX("screenshotinternal", screenshot);
      ADD_ACTION(setbrightness);
      ADD_ACTION(fileexists);
      ADD_ACTION(killterminal);
      ADD_ACTION(checkbackupname);
      ADD_ACTION(adbsideloadcancel);
      ADD_ACTION(startmtp);
      ADD_ACTION(stopmtp);
      ADD_ACTION(cancelbackup);
      ADD_ACTION(checkpartitionlifetimewrites);
      ADD_ACTION(mountsystemtoggle);
      ADD_ACTION(setlanguage);
      ADD_ACTION(togglebacklight);
      ADD_ACTION(enableadb);
      ADD_ACTION(enablefastboot);
      ADD_ACTION(disableled);
      ADD_ACTION(flashlight);

	  //fordownloads actions
      ADD_ACTION(fileextension);
      ADD_ACTION(up_a_level);
      ADD_ACTION(checkbackupfolder);
      ADD_ACTION(calculate_chmod);
      ADD_ACTION(get_chmod);
      ADD_ACTION(set_chmod);
      ADD_ACTION(setpassword);
      ADD_ACTION(passwordcheck);
 
      // remember actions that run in the caller thread
      for (mapFunc::const_iterator it = mf.begin(); it != mf.end(); ++it)
	setActionsRunningInCallerThread.insert(it->first);

      // These actions will run in a separate thread
      ADD_ACTION(flash);
      ADD_ACTION(wipe);
      ADD_ACTION(refreshsizes);
      ADD_ACTION(nandroid);
      ADD_ACTION(fixcontexts);
      ADD_ACTION(fixpermissions);
      ADD_ACTION(dd);
      ADD_ACTION(partitionsd);
      ADD_ACTION(installhtcdumlock);
      ADD_ACTION(htcdumlockrestoreboot);
      ADD_ACTION(htcdumlockreflashrecovery);
      ADD_ACTION(cmd);
      ADD_ACTION(terminalcommand);
      ADD_ACTION(reinjecttwrp);
      ADD_ACTION(decrypt);
      ADD_ACTION(adbsideload);
      ADD_ACTION(openrecoveryscript);
      ADD_ACTION(installsu);
      ADD_ACTION(fixsu);

      ADD_ACTION(decrypt_backup);
      ADD_ACTION(repair);
      ADD_ACTION(resize);
      ADD_ACTION(changefilesystem);
      ADD_ACTION(flashimage);
      ADD_ACTION(twcmd);
      ADD_ACTION(setbootslot);
      ADD_ACTION(repackimage);
      ADD_ACTION(fixabrecoverybootloop);
      ADD_ACTION(adb);
      ADD_ACTION(wlfw);
      ADD_ACTION(wlfx);
      ADD_ACTION(calldeactivateprocess);
      ADD_ACTION(disable_replace);

      //[f/d] Threaded actions
      ADD_ACTION(batch);
      ADD_ACTION(generatedigests);
      ADD_ACTION(ftls); //ftls (foxtools) - silent cmd
   }

  // First, get the action
  actions = FindNode(node, "actions");
  if (actions)
    child = FindNode(actions, "action");
  else
    child = FindNode(node, "action");

  if (!child)
    return;

  while (child)
    {
      Action action;

      attr = child->first_attribute("function");
      if (!attr)
	return;

      action.mFunction = attr->value();
      action.mArg = child->value();
      mActions.push_back(action);

      child = child->next_sibling("action");
    }

  // Now, let's get either the key or region
  child = FindNode(node, "touch");
  if (child)
    {
      attr = child->first_attribute("key");
      if (attr)
	{
	  std::vector < std::string > keys =
	    TWFunc::Split_String(attr->value(), "+");
	  for (size_t i = 0; i < keys.size(); ++i)
	    {
	      const int key = getKeyByName(keys[i]);
	      mKeys[key] = false;
	    }
	}
      else
	{
	  attr = child->first_attribute("x");
	  if (!attr)
	    return;
	  mActionX = atol(attr->value());
	  attr = child->first_attribute("y");
	  if (!attr)
	    return;
	  mActionY = atol(attr->value());
	  attr = child->first_attribute("w");
	  if (!attr)
	    return;
	  mActionW = atol(attr->value());
	  attr = child->first_attribute("h");
	  if (!attr)
	    return;
	  mActionH = atol(attr->value());
	}
    }
}

int GUIAction::NotifyTouch(TOUCH_STATE state, int x __unused, int y __unused)
{
  if (state == TOUCH_RELEASE)
    doActions();

  return 0;
}

int GUIAction::NotifyKey(int key, bool down)
{
  std::map < int, bool >::iterator itr = mKeys.find(key);
  if (itr == mKeys.end())
    return 1;

  bool prevState = itr->second;
  itr->second = down;

  // If there is only one key for this action, wait for key up so it
  // doesn't trigger with multi-key actions.
  // Else, check if all buttons are pressed, then consume their release events
  // so they don't trigger one-button actions and reset mKeys pressed status
  if (mKeys.size() == 1)
    {
      if (!down && prevState)
	{
	  doActions();
	  return 0;
	}
    }
  else if (down)
    {
      for (itr = mKeys.begin(); itr != mKeys.end(); ++itr)
	{
	  if (!itr->second)
	    return 1;
	}

      // Passed, all req buttons are pressed, reset them and consume release events
      HardwareKeyboard *kb = PageManager::GetHardwareKeyboard();
      for (itr = mKeys.begin(); itr != mKeys.end(); ++itr)
	{
	  kb->ConsumeKeyRelease(itr->first);
	  itr->second = false;
	}

      doActions();
      return 0;
    }

  return 1;
}

int GUIAction::NotifyVarChange(const std::string & varName,
			       const std::string & value)
{
  GUIObject::NotifyVarChange(varName, value);

  if (varName.empty() && !isConditionValid() && mKeys.empty() && !mActionW)
    doActions();
  else if ((varName.empty() || IsConditionVariable(varName))
	   && isConditionValid() && isConditionTrue())
    doActions();

  return 0;
}

void GUIAction::simulate_progress_bar(void)
{
  gui_msg("simulating=Simulating actions...");
  for (int i = 0; i < 5; i++)
    {
      if (PartitionManager.stop_backup.get_value())
	{
	  DataManager::SetValue("tw_cancel_backup", 1);
	  gui_msg("backup_cancel=Backup Cancelled");
	  DataManager::SetValue("ui_progress", 0);
	  PartitionManager.stop_backup.set_value(0);
	  return;
	}
      usleep(500000);
      DataManager::SetValue("ui_progress", i * 20);
    }
}

int GUIAction::flash_zip(std::string filename, int *wipe_cache)
{
  int ret_val = 0;

  DataManager::SetValue("ui_progress", 0);

  DataManager::SetValue("ui_portion_size", 0);
  DataManager::SetValue("ui_portion_start", 0);
  
  if (filename.empty())
    {
      LOGERR("No file specified.\n");
      return -1;
    }

  if (!TWFunc::Path_Exists(filename))
    {
      if (!PartitionManager.Mount_By_Path(filename, true))
	{
	  return -1;
	}
      if (!TWFunc::Path_Exists(filename))
	{
	  gui_msg(Msg(msg::kError, "unable_to_locate=Unable to locate {1}.")
		  (filename));
	  return -1;
	}
    }

  if (simulate)
    {
      simulate_progress_bar();
    }
  else
    {
      ret_val = TWinstall_zip(filename.c_str(), wipe_cache);
      PartitionManager.Unlock_Block_Partitions();

      // Now, check if we need to ensure TWRP remains installed...
      struct stat st;
      if (stat("/system/bin/installTwrp", &st) == 0)
	{
	  DataManager::SetValue("tw_operation", "Configuring TWRP");
	  DataManager::SetValue("tw_partition", "");
	  gui_msg("config_twrp=Configuring TWRP...");
	  if (TWFunc::Exec_Cmd("/system/bin/installTwrp reinstall") < 0)
	    {
	      gui_msg
		("config_twrp_err=Unable to configure TWRP with this kernel.");
	    }
	}

      //* DJ9
      Fox_Zip_Installer_Code = DataManager::GetIntValue(FOX_ZIP_INSTALLER_CODE);
      usleep(32);
      if (Fox_Zip_Installer_Code == 0) // this is a standard zip installer (not a ROM)
        {
           if (DataManager::GetIntValue(FOX_INSTALL_PREBUILT_ZIP) == 1)
              {
          	 LOGINFO("OrangeFox: processed internal zip: %s\n",filename.c_str());
              }
              else
          	 LOGINFO("OrangeFox: installed standard zip: %s\n",filename.c_str());
        }
      else // this is a ROM install
        {
          if (Fox_Zip_Installer_Code == 1) LOGINFO("OrangeFox: installed CUSTOM ROM: %s\n",filename.c_str());
          else
          if (Fox_Zip_Installer_Code == 2) LOGINFO("OrangeFox: installed MIUI ROM: %s\n",filename.c_str());
          else
          if (Fox_Zip_Installer_Code == 3) LOGINFO("OrangeFox: installed MIUI ROM and OTA_BAK: %s\n",filename.c_str());
          else
          if (Fox_Zip_Installer_Code == 11) LOGINFO("OrangeFox: installed Treble (Custom) ROM: %s\n",filename.c_str());
          else
          if (Fox_Zip_Installer_Code == 12) LOGINFO("OrangeFox: installed Treble (Custom) ROM and OTA_BAK: %s\n",filename.c_str());
          else
          if (Fox_Zip_Installer_Code == 22) LOGINFO("OrangeFox: installed Treble (MIUI) ROM: %s\n",filename.c_str());
          else
          if (Fox_Zip_Installer_Code == 23) LOGINFO("OrangeFox: installed Treble (MIUI) ROM and OTA_BAK: %s\n",filename.c_str());
          else
             LOGINFO("OrangeFox: installed Custom ROM: %s\n",filename.c_str());
          
          TWFunc::Dump_Current_Settings();
        }
       LOGINFO ("flash_zip: installer code = %i\n", DataManager::GetIntValue(FOX_ZIP_INSTALLER_CODE));
      //* DJ9
    }

  // Done
  DataManager::SetValue("ui_progress", 100);
  DataManager::SetValue("ui_progress", 0);
  DataManager::SetValue("ui_portion_size", 0);
  DataManager::SetValue("ui_portion_start", 0);
  return ret_val;
}

GUIAction::ThreadType GUIAction::getThreadType(const GUIAction::
					       Action & action)
{
  string func = gui_parse_text(action.mFunction);
  bool needsThread =
    setActionsRunningInCallerThread.find(func) ==
    setActionsRunningInCallerThread.end();
  if (needsThread)
    {
      if (func == "cancelbackup")
	return THREAD_CANCEL;
      else
	return THREAD_ACTION;
    }
  return THREAD_NONE;
}

int GUIAction::doActions()
{
  if (mActions.size() < 1)
    return -1;

  // Determine in which thread to run the actions.
  // Do it for all actions at once before starting, so that we can cancel the whole batch if the thread is already busy.
  ThreadType threadType = THREAD_NONE;
  std::vector < Action >::iterator it;
  for (it = mActions.begin(); it != mActions.end(); ++it)
    {
      ThreadType tt = getThreadType(*it);
      if (tt == THREAD_NONE)
	continue;
      if (threadType == THREAD_NONE)
	threadType = tt;
      else if (threadType != tt)
	{
	  LOGERR("Can't mix normal and cancel actions in the same list.\n"
		 "Running the whole batch in the cancel thread.\n");
	  threadType = THREAD_CANCEL;
	  break;
	}
    }

  // Now run the actions in the desired thread.
  switch (threadType)
    {
    case THREAD_ACTION:
      action_thread.threadActions(this);
      break;

    case THREAD_CANCEL:
      cancel_thread.threadActions(this);
      break;

    default:
      {
	// no iterators here because theme reloading might kill our object
	const size_t cnt = mActions.size();
	for (size_t i = 0; i < cnt; ++i)
	  doAction(mActions[i]);
      }
    }

  return 0;
}

int GUIAction::doAction(Action action)
{
  DataManager::GetValue(TW_SIMULATE_ACTIONS, simulate);

  std::string function = gui_parse_text(action.mFunction);
  std::string arg = gui_parse_text(action.mArg);

  // find function and execute it
  mapFunc::const_iterator funcitr = mf.find(function);
  if (funcitr != mf.end())
    return (this->*funcitr->second) (arg);
  
  if (! Hide_Reboot_Kludge_Fix(function))
  LOGERR("Unknown action '%s'\n", function.c_str());
  return -1;
}

void GUIAction::operation_start(const string operation_name)
{
	LOGINFO("operation_start: '%s'\n", operation_name.c_str());
	time(&Start);
	DataManager::SetValue(TW_ACTION_BUSY, 1);
	DataManager::SetValue("ui_progress", 0);
	DataManager::SetValue("ui_portion_size", 0);
	DataManager::SetValue("ui_portion_start", 0);
	DataManager::SetValue("tw_operation", operation_name);
	DataManager::SetValue("tw_operation_state", 0);
	DataManager::SetValue("tw_operation_status", 0);
	bool tw_ab_device = TWFunc::get_log_dir() != CACHE_LOGS_DIR;
	DataManager::SetValue("tw_ab_device", tw_ab_device);
}

void GUIAction::operation_end(const int operation_status)
{
	time_t Stop;
	int simulate_fail;
	DataManager::SetValue("ui_progress", 100);
	if (simulate) {
		DataManager::GetValue(TW_SIMULATE_FAIL, simulate_fail);
		if (simulate_fail != 0)
			DataManager::SetValue("tw_operation_status", 1);
		else
			DataManager::SetValue("tw_operation_status", 0);
	} else {
		if (operation_status != 0) {
			DataManager::SetValue("tw_operation_status", 1);
		}
		else {
			DataManager::SetValue("tw_operation_status", 0);
		}
	}
	DataManager::SetValue("tw_operation_state", 1);
	DataManager::SetValue(TW_ACTION_BUSY, 0);
	blankTimer.resetTimerAndUnblank();
	property_set("orangefox.action_complete", "1");
	time(&Stop);

#ifndef TW_NO_HAPTICS
	if ((int) difftime(Stop, Start) > 10)
		DataManager::Vibrate("tw_action_vibrate");
#endif

	LOGINFO("operation_end - status=%d\n", operation_status);
}

int GUIAction::reboot(std::string arg)
{
  sync();
  DataManager::SetValue("tw_gui_done", 1);
  DataManager::SetValue("tw_reboot_arg", arg);
  return 0;
}

int GUIAction::home(std::string arg __unused)
{
  gui_changePage("main");
  return 0;
}

int GUIAction::key(std::string arg)
{
  const int key = getKeyByName(arg);
  PageManager::NotifyKey(key, true);
  PageManager::NotifyKey(key, false);
  return 0;
}

int GUIAction::page(std::string arg)
{
  property_set("orangefox.action_complete", "0");
  std::string page_name = gui_parse_text(arg);
  return gui_changePage(page_name);
}

int GUIAction::reload(std::string arg __unused)
{
  PageManager::RequestReload();
  // The actual reload is handled in pages.cpp in PageManager::RunReload()
  // The reload will occur on the next Update or Render call and will
  // be performed in the rendoer thread instead of the action thread
  // to prevent crashing which could occur when we start deleting
  // GUI resources in the action thread while we attempt to render
  // with those same resources in another thread.
  return 0;
}

// [f/d] pass hashing actions: hash new pass / hash entered pass 
int GUIAction::setpassword(std::string arg __unused)
{
  char sum[129];

  // string to char
  string pass_tmp2 = DataManager::GetStrValue("pass_new_1");
  char pass_tmp[pass_tmp2.length() + 1];
  strcpy(pass_tmp, pass_tmp2.c_str());

  sha512sum(pass_tmp, sum);

  //char to string
  string pass_tmp3(sum); 

  DataManager::SetValue("fox_pass_true", pass_tmp3);
  return 0;
}

int GUIAction::passwordcheck(std::string arg __unused)
{
  char sum[129];
  string pass_tmp2 = DataManager::GetStrValue("pass_enter");
  char pass_tmp[pass_tmp2.length() + 1];
  strcpy(pass_tmp, pass_tmp2.c_str());

  sha512sum(pass_tmp, sum);

  string pass_tmp3(sum); 
  
  DataManager::SetValue("pass_enter_hash", pass_tmp3);
  gui_changePage("password_check");
  return 0;
}

void GUIAction::sha512sum(char *string, char outputBuffer[129])
{
    #ifdef OF_LEGACY_SHAR512
    twrpSHA512 myshar;
    myshar.update((const unsigned char *)string, strlen(string));
    std::string s = myshar.return_digest_string();
    strcpy(outputBuffer, s.c_str());
    #else
    int i = 0;
    unsigned char hash[SHA512_DIGEST_LENGTH];
    SHA512_CTX sha512;
    SHA512_Init(&sha512);
    SHA512_Update(&sha512, string, strlen(string));
    SHA512_Final(hash, &sha512);
    for(i = 0; i < SHA512_DIGEST_LENGTH; i++)
    {
        sprintf(outputBuffer + (i * 2), "%02x", hash[i]);
    }
    #endif
    outputBuffer[64] = 0;
}

int GUIAction::check_and_reload(std::string arg __unused)
{
  if (!DataManager::GetIntValue("of_decrypt_from_menu")) //int == 0
    return 0;
  
  if (TWFunc::Path_Exists(Fox_Home + "/.theme") || TWFunc::Path_Exists(Fox_Home + "/.navbar")) {
    PageManager::RequestReload();
    gui_changePage("reapply_settings");
  } else {
    //skip reload, just go to page
    gui_changePage(DataManager::GetStrValue("of_reload_back"));
  }
  return 0;
}

int GUIAction::readBackup(std::string arg __unused)
{
  string Restore_Name;
  DataManager::GetValue("tw_restore", Restore_Name);
  PartitionManager.Set_Restore_Files(Restore_Name);
  return 0;
}

// Convert user's chmod number (e.g 0755) to checkboxes
int GUIAction::set_chmod(std::string arg __unused)
{
    int checkId = 1;
    string renamevar = DataManager::GetStrValue("tw_filemanager_rename");

    for(char& c : std::string(4 - renamevar.length(), '0') + renamevar) {
        int num = c - '0';
        for(int val : { 4, 2, 1 }) {
          DataManager::SetValue("chmod_id" + std::to_string(checkId), num >= val ? 1 : 0);
          if (num >= val)
            num -= val;
          checkId++;
        }
    }
    return 0;
}

// Get perms of tw_filename1
int GUIAction::get_chmod(std::string arg __unused)
{
    struct stat st;
    if(stat(DataManager::GetStrValue("tw_filename1").c_str(), &st) == 0) {
      mode_t perm = st.st_mode;
      int statchmod = 0;
      #define sch(c,n) statchmod += (perm & c) ? n : 0
      sch(S_IRUSR,400); sch(S_IRGRP,40); sch(S_IROTH,4);
      sch(S_IWUSR,200); sch(S_IWGRP,20); sch(S_IWOTH,2);
      sch(S_IXUSR,100); sch(S_IXGRP,10); sch(S_IXOTH,1);
      sch(S_ISUID,4000); sch(S_ISGID,2000); sch(S_ISVTX,1000);
      
      DataManager::SetValue("tw_filemanager_rename", statchmod);
    } else {
      LOGINFO("Error while reading file perms; skipping");
    }
    return 0;
}

// Convert checkboxes to checkboxes chmod number
int GUIAction::calculate_chmod(std::string arg __unused)
{
  int mult = 1, checkId = 12, chmodval = 0;
  while (mult <= 1000) {
    for(int val : { 1, 2, 4 }) {
      chmodval += DataManager::GetIntValue("chmod_id" + std::to_string(checkId)) * val * mult;
      checkId--;
    }
    mult *= 10;
  }
  
  DataManager::SetValue("tw_filemanager_rename", std::string(4 - std::to_string(chmodval).length(), '0') + std::to_string(chmodval));
  return 0;
}

int GUIAction::checkbackupfolder(std::string arg __unused)
{
  string path;
  DIR* d;
  struct stat s;
	struct dirent* de;

	DataManager::GetValue(TW_BACKUPS_FOLDER_VAR, path);
  DataManager::SetValue("of_backup_rw", "1");
  DataManager::SetValue("of_backup_empty", "1");

  // create backup folder if it not exist
  // if we failed to create folder, user possibly trying to open usb with ntfs
  string dirpath = path + "/.";
  if( stat(dirpath.c_str(),&s) != 0 )
    if (!TWFunc::Recursive_Mkdir(path, false))
      DataManager::SetValue("of_backup_rw", "0");

  // open dir and trying to get file list
  d = opendir(path.c_str());
	if (d != NULL) {
      while ((de = readdir(d)) != NULL) {
        std::string name = de->d_name;
        // skip special folders
        if (name == "." || name == "..")
          continue;

        unsigned char type = de->d_type;
        if (type == DT_UNKNOWN)
			    type = TWFunc::Get_D_Type_From_Stat(path);
        if (type != DT_DIR)
          continue;
        
        // when we found a normal folder, set of_backup_empty to 0 and exit loop
        DataManager::SetValue("of_backup_empty", "0");
        break;
      }
      closedir(d);
  } else {
      LOGINFO("Error opening folder: %s\n", path.c_str());
  }

  DataManager::SetValue("tw_backups_folder_fm", path);
  // switch to restore_prep page
  gui_changePage("restore_prep");

  return 0;
}

int GUIAction::set(std::string arg)
{
  if (arg.find('=') != string::npos)
    {
      string varName = arg.substr(0, arg.find('='));
      string value = arg.substr(arg.find('=') + 1, string::npos);

      DataManager::GetValue(value, value);
      DataManager::SetValue(varName, value);
    }
  else
    DataManager::SetValue(arg, "1");
  return 0;
}

int GUIAction::clear(std::string arg)
{
  DataManager::SetValue(arg, "0");
  return 0;
}

int GUIAction::up_a_level(std::string arg)
{
    string fm_path;
    DataManager::GetValue(arg, fm_path);
	size_t found;
	found = fm_path.find_last_of('/');
	if (found != string::npos) {
		string new_folder = fm_path.substr(0, found);
		if (new_folder.length() < 2)
			new_folder = "/";
		DataManager::SetValue(arg, new_folder);
	}
	return 0;
}

int GUIAction::fileextension(std::string arg)
{
  DataManager::SetValue("tw_file_extension", TWFunc::lowercase(arg.substr(arg.find_last_of(".") + 1)));
  return 0;
}

int GUIAction::mount(std::string arg)
{
  if (arg == "usb")
    {
      DataManager::SetValue(TW_ACTION_BUSY, 1);
      if (!simulate)
	PartitionManager.usb_storage_enable();
      else
	gui_msg("simulating=Simulating actions...");
    }
  else if (!simulate)
    {
      PartitionManager.Mount_By_Path(arg, true);
      PartitionManager.Add_MTP_Storage(arg);
    }
  else
    gui_msg("simulating=Simulating actions...");
  return 0;
}

int GUIAction::unmount(std::string arg)
{
  if (arg == "usb")
    {
      if (!simulate)
	PartitionManager.usb_storage_disable();
      else
	gui_msg("simulating=Simulating actions...");
      DataManager::SetValue(TW_ACTION_BUSY, 0);
    }
  else if (!simulate)
    {
      PartitionManager.UnMount_By_Path(arg, true);
    }
  else
    gui_msg("simulating=Simulating actions...");
  return 0;
}

int GUIAction::restoredefaultsettings(std::string arg __unused)
{
  operation_start("Restore Defaults");
  if (simulate)			// Simulated so that people don't accidently wipe out the "simulation is on" setting
    gui_msg("simulating=Simulating actions...");
  else
    {
      DataManager::ResetDefaults();
      PartitionManager.Update_System_Details();
      PartitionManager.Mount_Current_Storage(true);
    }
  operation_end(0);
  return 0;
}

int GUIAction::copylog(std::string arg __unused)
{
  operation_start("Copy Log");
  if (!simulate)
    {
      string dst, curr_storage;
      int copy_kernel_log = 0;

      DataManager::GetValue("tw_include_kernel_log", copy_kernel_log);
      PartitionManager.Mount_Current_Storage(true);
      curr_storage = DataManager::GetCurrentStoragePath();
      dst = curr_storage + "/recovery.log";
      TWFunc::copy_file("/tmp/recovery.log", dst.c_str(), 0755);
      tw_set_default_metadata(dst.c_str());
      if (copy_kernel_log)
	TWFunc::copy_kernel_log(curr_storage);
      sync();
      gui_msg(Msg("copy_log=Copied recovery log to {1}") (dst));
    }
  else
    simulate_progress_bar();
  operation_end(0);
  return 0;
}


int GUIAction::compute(std::string arg)
{
  if (arg.find("+") != string::npos)
    {
      string varName = arg.substr(0, arg.find('+'));
      string string_to_add = arg.substr(arg.find('+') + 1, string::npos);
      int amount_to_add = atoi(string_to_add.c_str());
      int value;

      DataManager::GetValue(varName, value);
      DataManager::SetValue(varName, value + amount_to_add);
      return 0;
    }
  if (arg.find("-") != string::npos)
    {
      string varName = arg.substr(0, arg.find('-'));
      string string_to_subtract = arg.substr(arg.find('-') + 1, string::npos);
      int amount_to_subtract = atoi(string_to_subtract.c_str());
      int value;

      DataManager::GetValue(varName, value);
      value -= amount_to_subtract;
      if (value <= 0)
	value = 0;
      DataManager::SetValue(varName, value);
      return 0;
    }
  if (arg.find("*") != string::npos)
    {
      string varName = arg.substr(0, arg.find('*'));
      string multiply_by_str =
	gui_parse_text(arg.substr(arg.find('*') + 1, string::npos));
      int multiply_by = atoi(multiply_by_str.c_str());
      int value;

      DataManager::GetValue(varName, value);
      DataManager::SetValue(varName, value * multiply_by);
      return 0;
    }
  if (arg.find("/") != string::npos)
    {
      string varName = arg.substr(0, arg.find('/'));
      string divide_by_str =
	gui_parse_text(arg.substr(arg.find('/') + 1, string::npos));
      int divide_by = atoi(divide_by_str.c_str());
      int value;

      if (divide_by != 0)
	{
	  DataManager::GetValue(varName, value);
	  DataManager::SetValue(varName, value / divide_by);
	}
      return 0;
    }
  LOGERR("Unable to perform compute '%s'\n", arg.c_str());
  return -1;
}

int GUIAction::setguitimezone(std::string arg __unused)
{
  string SelectedZone;
  DataManager::GetValue(TW_TIME_ZONE_GUISEL, SelectedZone);	// read the selected time zone into SelectedZone
  string Zone = SelectedZone.substr(0, SelectedZone.find(';'));	// parse to get time zone
  string DSTZone = SelectedZone.substr(SelectedZone.find(';') + 1, string::npos);	// parse to get DST component

  int dst;
  DataManager::GetValue(TW_TIME_ZONE_GUIDST, dst);	// check wether user chose to use DST

  string offset;
  DataManager::GetValue(TW_TIME_ZONE_GUIOFFSET, offset);	// pull in offset

  string NewTimeZone = Zone;
  if (offset != "0")
    NewTimeZone += ":" + offset;

  if (dst != 0)
    NewTimeZone += DSTZone;

  DataManager::SetValue(TW_TIME_ZONE_VAR, NewTimeZone);
  DataManager::update_tz_environment_variables();
  return 0;
}

int GUIAction::overlay(std::string arg)
{
  return gui_changeOverlay(arg);
}

int GUIAction::queuezip(std::string arg __unused)
{
  if (zip_queue_index >= 10)
    {
      gui_msg("max_queue=Maximum zip queue reached!");
      return 0;
    }
  DataManager::GetValue("tw_filename", zip_queue[zip_queue_index]);
  if (strlen(zip_queue[zip_queue_index].c_str()) > 0)
    {
      zip_queue_index++;
      DataManager::SetValue(TW_ZIP_QUEUE_COUNT, zip_queue_index);
      DataManager::SetValue("tw_q_" + to_string(zip_queue_index), DataManager::GetStrValue("tw_file"));
      find_magisk();
    }
    
  return 0;
}

void GUIAction::find_magisk(){ //[f/d]
  int found = 0;
  for (int i = 0; i < zip_queue_index; i++)
    if (zip_queue[i] == Fox_Home_Files + "/Magisk.zip")
      found = 1;
  DataManager::SetValue("of_magisk_in_queue", found);
}

int GUIAction::cancelzip(std::string arg __unused)
{
  if (zip_queue_index <= 0)
    {
      gui_msg("min_queue=Minimum zip queue reached!");
      return 0;
    }
  else
    {
      DataManager::SetValue("tw_q_" + to_string(zip_queue_index), "");
      zip_queue_index--;
      string zip_path = zip_queue[zip_queue_index - 1];
      size_t slashpos = zip_path.find_last_of('/');
      DataManager::SetValue("tw_zip_location", zip_path.substr(0, slashpos));
      DataManager::SetValue("tw_file",
        (slashpos == string::npos) ? zip_path : zip_path.substr(slashpos + 1));
      find_magisk();

      DataManager::SetValue(TW_ZIP_QUEUE_COUNT, zip_queue_index);
    }
  return 0;
}

int GUIAction::queueclear(std::string arg __unused)
{
  zip_queue_index = 0;
  DataManager::SetValue(TW_ZIP_QUEUE_COUNT, zip_queue_index);
  return 0;
}

int GUIAction::sleep(std::string arg)
{
  operation_start("Sleep");
  usleep(atoi(arg.c_str()));
  operation_end(0);
  return 0;
}

int GUIAction::sleepcounter(std::string arg)
{
  operation_start("SleepCounter");
  // Ensure user notices countdown in case it needs to be cancelled
  blankTimer.resetTimerAndUnblank();
  int total = atoi(arg.c_str());
  for (int t = total; t > 0; t--)
    {
      int progress = (int) (((float) (total - t) / (float) total) * 100.0);
      DataManager::SetValue("ui_progress", progress);
      ::sleep(1);
      DataManager::SetValue("tw_sleep", t - 1);
    }
  DataManager::SetValue("ui_progress", 100);
  operation_end(0);
  return 0;
}

int GUIAction::appenddatetobackupname(std::string arg __unused)
{
  operation_start("AppendDateToBackupName");
  string Backup_Name;
  DataManager::GetValue(TW_BACKUP_NAME, Backup_Name);
  Backup_Name += TWFunc::Get_Current_Date();
  if (Backup_Name.size() > MAX_BACKUP_NAME_LEN)
    Backup_Name.resize(MAX_BACKUP_NAME_LEN);
  DataManager::SetValue(TW_BACKUP_NAME, Backup_Name);
  PageManager::NotifyKey(KEY_END, true);
  PageManager::NotifyKey(KEY_END, false);
  operation_end(0);
  return 0;
}

int GUIAction::generatebackupname(std::string arg __unused)
{
  operation_start("GenerateBackupName");
  TWFunc::Auto_Generate_Backup_Name();
  operation_end(0);
  return 0;
}

int GUIAction::checkpartitionlist(std::string arg)
{
  string List, part_path;
  int count = 0;

  if (arg.empty())
    arg = "part_option";
  DataManager::GetValue(arg, List);
  LOGINFO("checkpartitionlist list '%s'\n", List.c_str());
  if (!List.empty())
    {
      size_t start_pos = 0, end_pos = List.find(";", start_pos);
      while (end_pos != string::npos && start_pos < List.size())
	{
	  part_path = List.substr(start_pos, end_pos - start_pos);
	  LOGINFO("checkpartitionlist part_path '%s'\n", part_path.c_str());
	  if (part_path == "/and-sec" || part_path == "DALVIK"
	      || part_path == "INTERNAL" || part_path == "SUBSTRATUM")
	    {
	      // Do nothing
	    }
	  else
	    {
	      count++;
	    }
	  start_pos = end_pos + 1;
	  end_pos = List.find(";", start_pos);
	}
      DataManager::SetValue("tw_check_partition_list", count);
    }
  else
    {
      DataManager::SetValue("tw_check_partition_list", 0);
    }
  return 0;
}

int GUIAction::getpartitiondetails(std::string arg)
{
  string List, part_path;

  if (arg.empty())
    arg = "part_option";
  DataManager::GetValue(arg, List);
  LOGINFO("getpartitiondetails list '%s'\n", List.c_str());
  if (!List.empty())
    {
      size_t start_pos = 0, end_pos = List.find(";", start_pos);
      part_path = List;
      while (end_pos != string::npos && start_pos < List.size())
	{
	  part_path = List.substr(start_pos, end_pos - start_pos);
	  LOGINFO("getpartitiondetails part_path '%s'\n", part_path.c_str());
	  if (part_path == "/and-sec" || part_path == "DALVIK" || part_path == "INTERNAL")
	    {
	      // Do nothing
	    }
	  else
	    {
	      DataManager::SetValue("tw_partition_path", part_path);
	      break;
	    }
	  start_pos = end_pos + 1;
	  end_pos = List.find(";", start_pos);
	}
      if (!part_path.empty())
	{
	  TWPartition *Part =
	    PartitionManager.Find_Partition_By_Path(part_path);
	  if (Part)
	    {
	      unsigned long long mb = 1048576;

	      DataManager::SetValue("tw_partition_name", Part->Display_Name);
	      DataManager::SetValue("tw_partition_mount_point",
				    Part->Mount_Point);
	      DataManager::SetValue("tw_partition_file_system",
				    Part->Current_File_System);
	      DataManager::SetValue("tw_partition_size", Part->Size / mb);
	      DataManager::SetValue("tw_partition_used", Part->Used / mb);
	      DataManager::SetValue("tw_partition_free", Part->Free / mb);
	      DataManager::SetValue("tw_partition_backup_size",
				    Part->Backup_Size / mb);
	      DataManager::SetValue("tw_partition_removable",
				    Part->Removable);
	      DataManager::SetValue("tw_partition_is_present",
				    Part->Is_Present);

	      if (Part->Can_Repair())
		DataManager::SetValue("tw_partition_can_repair", 1);
	      else
		DataManager::SetValue("tw_partition_can_repair", 0);
	      if (Part->Can_Resize())
		DataManager::SetValue("tw_partition_can_resize", 1);
	      else
		DataManager::SetValue("tw_partition_can_resize", 0);
	      if (TWFunc::Path_Exists(Fox_Bin_Dir + "/mkfs.fat"))
		DataManager::SetValue("tw_partition_vfat", 1);
	      else
		DataManager::SetValue("tw_partition_vfat", 0);
	      if (TWFunc::Path_Exists(Fox_Bin_Dir + "/mkexfatfs"))
		DataManager::SetValue("tw_partition_exfat", 1);
	      else
		DataManager::SetValue("tw_partition_exfat", 0);
	      if (TWFunc::Path_Exists(Fox_Bin_Dir + "/mkfs.f2fs") || TWFunc::Path_Exists(Fox_Bin_Dir + "/make_f2fs"))
		DataManager::SetValue("tw_partition_f2fs", 1);
	      else
		DataManager::SetValue("tw_partition_f2fs", 0);
	      if (TWFunc::Path_Exists(Fox_Bin_Dir + "/mke2fs"))
		DataManager::SetValue("tw_partition_ext", 1);
	      else
		DataManager::SetValue("tw_partition_ext", 0);
	      return 0;
	    }
	  else
	    {
	      LOGERR("Unable to locate partition: '%s'\n", part_path.c_str());
	    }
	}
    }
  DataManager::SetValue("tw_partition_name", "");
  DataManager::SetValue("tw_partition_file_system", "");
  // Set this to 0 to prevent trying to partition this device, just in case
  DataManager::SetValue("tw_partition_removable", 0);
  return 0;
}

int GUIAction::screenshot(std::string arg __unused)
{
	time_t tm;
	char path[256];
	int path_len;
	uid_t uid = AID_MEDIA_RW;
	gid_t gid = AID_MEDIA_RW;

	//const std::string storage = DataManager::GetCurrentStoragePath();
	//if (PartitionManager.Is_Mounted_By_Path(storage)) {
	//	snprintf(path, sizeof(path), "%s/Fox/screenshots/", storage.c_str());
	//} else
	strcpy(path, "/sdcard/Fox/screenshots/");

	if (!TWFunc::Create_Dir_Recursive(path, 0775, uid, gid))
		return 0;

	tm = time(NULL);
	path_len = strlen(path);

	// Screenshot_2014-01-01-18-21-38.png
	strftime(path+path_len, sizeof(path)-path_len, "Screenshot_%Y-%m-%d-%H-%M-%S.png", localtime(&tm));

	int res = gr_save_screenshot(path);
	if (res == 0) {
		chmod(path, 0666);
		chown(path, uid, gid);

		gui_msg(Msg("screenshot_saved=Screenshot was saved to {1}")(path));

		// blink to notify that the screenshot was taken
		gr_color(255, 255, 255, 255);
		gr_fill(0, 0, gr_fb_width(), gr_fb_height());
		gr_flip();
		gui_forceRender();
	} else {
		gui_err("screenshot_err=Failed to take a screenshot!");
	}
	return 0;
}

int GUIAction::setbrightness(std::string arg)
{
  return TWFunc::Set_Brightness(arg);
}

int GUIAction::fileexists(std::string arg)
{
  struct stat st;
  string newpath = arg + "/.";

  operation_start("FileExists");
  if (stat(arg.c_str(), &st) == 0 || stat(newpath.c_str(), &st) == 0)
    operation_end(0);
  else
    operation_end(1);
  return 0;
}

void GUIAction::reinject_after_flash()
{
  if (DataManager::GetIntValue(TW_HAS_INJECTTWRP) == 1
      && DataManager::GetIntValue(TW_INJECT_AFTER_ZIP) == 1)
    {
      gui_msg("injecttwrp=Injecting TWRP into boot image...");
      if (simulate)
	{
	  simulate_progress_bar();
	}
      else
	{
	  TWPartition *Boot = PartitionManager.Find_Partition_By_Path("/boot");
	  if (Boot == NULL || Boot->Current_File_System != "emmc")
	    TWFunc::
	      Exec_Cmd
	      ("injecttwrp --dump /tmp/backup_recovery_ramdisk.img /tmp/injected_boot.img --flash");
	  else
	    {
	      string injectcmd =
		"injecttwrp --dump /tmp/backup_recovery_ramdisk.img /tmp/injected_boot.img --flash bd="
		+ Boot->Actual_Block_Device;
	      TWFunc::Exec_Cmd(injectcmd);
	    }
	  gui_msg("done=Done.");
	}
    }
}

int GUIAction::ozip_decrypt(string zip_path)
{
   if (!TWFunc::Path_Exists(Fox_Bin_Dir + "/ozip_decrypt"))
      {
         return 1;
      }
   gui_msg("ozip_decrypt_decryption=Starting Ozip Decryption...");
   TWFunc::Exec_Cmd("ozip_decrypt " + (string)TW_OZIP_DECRYPT_KEY + " '" + zip_path + "'");
   gui_msg("ozip_decrypt_finish=Ozip Decryption Finished!");
   return 0;
}

int GUIAction::flash(std::string arg)
{
  int i, ret_val = 0, wipe_cache = 0;

  // We're going to jump to this page first, like a loading page
  gui_changePage(arg);

  // loop through the zip(s) to be installed
  for (i = 0; i < zip_queue_index; i++)
    {
      string zip_path = zip_queue[i];
      size_t slashpos = zip_path.find_last_of('/');
      string zip_filename = (slashpos == string::npos) ? zip_path : zip_path.substr(slashpos + 1);
      operation_start("Flashing");

      if ((zip_path.substr(zip_path.size() - 4, 4)) == "ozip")
	{
		if ((ozip_decrypt(zip_path)) != 0)
		  {
                	LOGERR("Unable to find ozip_decrypt!");
			break;
		  }
		zip_filename = (zip_filename.substr(0, zip_filename.size() - 4)).append("zip");
            	zip_path = (zip_path.substr(0, zip_path.size() - 4)).append("zip");
		if (!TWFunc::Path_Exists(zip_path)) {
			LOGERR("Unable to find decrypted zip");
			break;
		}
	}

      DataManager::SetValue("tw_filename", zip_path);
      DataManager::SetValue("tw_file", zip_filename);
      DataManager::SetValue(TW_ZIP_INDEX, (i + 1));

      TWFunc::SetPerformanceMode(true);

      // try to flash the zip
      ret_val = flash_zip(zip_path, &wipe_cache);
      TWFunc::SetPerformanceMode(false);

      // what was the outcome ?
      if (ret_val != 0)
	{
	  gui_msg(Msg(msg::kError, "zip_err=Error installing zip file '{1}'") (zip_path));
	  //gui_print_color("error", "Hmmm ... does someone have a patched boot image?\n");
	  ret_val = 1;
	  break;
	}

      // success - but what have we just installed?
      if (Fox_Zip_Installer_Code != 0) // we have just installed a ROM - ideally, the user should reboot the recovery
       {
          Fox_Post_Zip_Install(INSTALL_SUCCESS);
       }

       usleep(250000);
     } // for i

   zip_queue_index = 0;

   if (wipe_cache)
    {
      gui_msg("zip_wipe_cache=One or more zip requested a cache wipe -- Wiping cache now.");
      PartitionManager.Wipe_By_Path("/cache");
    }

   DataManager::Vibrate("tw_action_vibrate");
   DataManager::Leds(true);

   reinject_after_flash(); // ** redundant code
   PartitionManager.Update_System_Details();
   operation_end(ret_val);
   DataManager::SetValue(FOX_INSTALL_PREBUILT_ZIP, 0); // if we have installed an internal zip, turn off the flag

   // This needs to be after the operation_end call so we change pages before we change variables that we display on the screen
   DataManager::SetValue(TW_ZIP_QUEUE_COUNT, zip_queue_index);

   return 0;
}

int GUIAction::wipe(std::string arg)
{
  operation_start("Format");
  DataManager::SetValue("tw_partition", arg);

  int ret_val = false;

  if (simulate)
    {
      simulate_progress_bar();
    }
  else
    {
      if (arg == "data")
	ret_val = PartitionManager.Factory_Reset();
      else if (arg == "battery")
	ret_val = PartitionManager.Wipe_Battery_Stats();
      else if (arg == "rotate")
	ret_val = PartitionManager.Wipe_Rotate_Data();
      else if (arg == "dalvik")
	ret_val = PartitionManager.Wipe_Dalvik_Cache();
      else if (arg == "DATAMEDIA")
	{
	  ret_val = PartitionManager.Format_Data();
	}
      else if (arg == "INTERNAL")
	{
	  int has_datamedia;

	  DataManager::GetValue(TW_HAS_DATA_MEDIA, has_datamedia);
	  if (has_datamedia)
	    {
	      ret_val = PartitionManager.Wipe_Media_From_Data();
	    }
	  else
	    {
	      ret_val =
		PartitionManager.
		Wipe_By_Path(DataManager::GetSettingsStoragePath());
	    }
	}
      else if (arg == "EXTERNAL")
	{
	  string External_Path;

	  DataManager::GetValue(TW_EXTERNAL_PATH, External_Path);
	  ret_val = PartitionManager.Wipe_By_Path(External_Path);
	}
      else if (arg == "ANDROIDSECURE")
	{
	  ret_val = PartitionManager.Wipe_Android_Secure();
	}
      else if (arg == "LIST")
	{
	  string Wipe_List, wipe_path;
	  bool skip = false;
	  ret_val = true;

	  DataManager::GetValue("tw_wipe_list", Wipe_List);
	  LOGINFO("wipe list '%s'\n", Wipe_List.c_str());
	  if (!Wipe_List.empty())
	    {
	      size_t start_pos = 0, end_pos = Wipe_List.find(";", start_pos);
	      while (end_pos != string::npos && start_pos < Wipe_List.size())
		{
		  wipe_path =
		    Wipe_List.substr(start_pos, end_pos - start_pos);
		  LOGINFO("wipe_path '%s'\n", wipe_path.c_str());
		  if (wipe_path == "/and-sec")
		    {
		      if (!PartitionManager.Wipe_Android_Secure())
			{
			  gui_msg
			    ("and_sec_wipe_err=Unable to wipe android secure");
			  ret_val = false;
			  break;
			}
		      else
			{
			  skip = true;
			}
		    }
		  else if (wipe_path == "DALVIK")
		    {
		      if (!PartitionManager.Wipe_Dalvik_Cache())
			{
			  gui_err("dalvik_wipe_err=Failed to wipe dalvik");
			  ret_val = false;
			  break;
			}
		      else
			{
			  skip = true;
			}
		    }
		  else if (wipe_path == "INTERNAL")
		    {
		      if (!PartitionManager.Wipe_Media_From_Data())
			{
			  ret_val = false;
			  break;
			}
		      else
			{
			  skip = true;
			}
		    }
		  if (!skip)
		    {
		      if (!PartitionManager.Wipe_By_Path(wipe_path))
			{
			  gui_msg(Msg
				  (msg::kError,
				   "unable_to_wipe=Unable to wipe {1}.")
				  (wipe_path));
			  ret_val = false;
			  break;
			}
		      else if (wipe_path ==
			       DataManager::GetSettingsStoragePath())
			{
			  arg = wipe_path;
			}
		    }
		  else
		    {
		      skip = false;
		    }
		  start_pos = end_pos + 1;
		  end_pos = Wipe_List.find(";", start_pos);
		}
	    }
	}
      else
	ret_val = PartitionManager.Wipe_By_Path(arg);
#ifndef TW_OEM_BUILD
      if (arg == DataManager::GetSettingsStoragePath())
	{
	  // If we wiped the settings storage path, recreate the TWRP folder and dump the settings
	  string Storage_Path = DataManager::GetSettingsStoragePath();

	  if (PartitionManager.Mount_By_Path(Storage_Path, true))
	    {
	      LOGINFO("Making TWRP folder and saving settings.\n");
	      Storage_Path += "/Fox";
	      mkdir(Storage_Path.c_str(), 0777);
	      DataManager::Flush();
	    }
	  else
	    {
	      LOGERR("Unable to recreate TWRP folder and save settings.\n");
	    }
	}
#endif
    }
  PartitionManager.Update_System_Details();
  if (ret_val)
    ret_val = 0;		// 0 is success
  else
    ret_val = 1;		// 1 is failure
  operation_end(ret_val);
  return 0;
}

int GUIAction::refreshsizes(std::string arg __unused)
{
  operation_start("Refreshing Sizes");
  if (simulate)
    {
      simulate_progress_bar();
    }
  else
    PartitionManager.Update_System_Details();
  operation_end(0);
  return 0;
}

int GUIAction::nandroid(std::string arg)
{
	if (simulate) {
		PartitionManager.stop_backup.set_value(0);
		DataManager::SetValue("tw_partition", "Simulation");
		simulate_progress_bar();
		operation_end(0);
	} else {
		operation_start("Nandroid");
		int ret = 0;

		if (arg == "backup") {
			string Backup_Name;
			DataManager::GetValue(TW_BACKUP_NAME, Backup_Name);
			string auto_gen = gui_lookup("auto_generate", "(Auto Generate)");
			if (Backup_Name == auto_gen || Backup_Name == gui_lookup("curr_date", "(Current Date)") || Backup_Name == "0" || Backup_Name == "(" || PartitionManager.Check_Backup_Name(Backup_Name, true, true) == 0) {
				ret = PartitionManager.Run_Backup(false);
				DataManager::SetValue("tw_encrypt_backup", 0); // reset value so we don't encrypt every subsequent backup
				if (!PartitionManager.stop_backup.get_value()) {
					if (ret == false)
						ret = 1; // 1 for failure
					else
						ret = 0; // 0 for success
					DataManager::SetValue("tw_cancel_backup", 0);
				} else {
					DataManager::SetValue("tw_cancel_backup", 1);
					gui_msg("backup_cancel=Backup Cancelled");
					ret = 0;
				}
			} else {
				operation_end(1);
				return -1;
			}
			DataManager::SetValue(TW_BACKUP_NAME, auto_gen);
		} else if (arg == "restore") {
			string Restore_Name;
			int gui_adb_backup;

			DataManager::GetValue("tw_restore", Restore_Name);
			DataManager::GetValue("tw_enable_adb_backup", gui_adb_backup);
			if (gui_adb_backup) {
				DataManager::SetValue("tw_operation_state", 1);
				if (TWFunc::stream_adb_backup(Restore_Name) == 0)
					ret = 0; // success
				else
					ret = 1; // failure
				DataManager::SetValue("tw_enable_adb_backup", 0);
				ret = 0; // assume success???
			} else {
				if (PartitionManager.Run_Restore(Restore_Name))
					ret = 0; // success
				else
					ret = 1; // failure
			}
		} else {
			operation_end(1); // invalid arg specified, fail
			return -1;
		}
		operation_end(ret);
		return ret;
	}
	return 0;
}


int GUIAction::cancelbackup(std::string arg __unused)
{
  if (simulate)
    {
      PartitionManager.stop_backup.set_value(1);
    }
  else
    {
      int op_status = PartitionManager.Cancel_Backup();
      if (op_status != 0)
	op_status = 1;		// failure
    }

  return 0;
}

int GUIAction::generatedigests(std::string arg __unused)
{
  int op_status = 0;

  //Generate digests for latest backup
  operation_start("Generate digests");
  op_status = twrpDigestDriver::Run_Digest();
  operation_end(op_status);

  return 0;
}

int GUIAction::fixcontexts(std::string arg __unused)
{
  int op_status = 0;

  operation_start("Fix Contexts");
  LOGINFO("fix contexts started!\n");
  if (simulate)
    {
      simulate_progress_bar();
    }
  else
    {
      op_status = PartitionManager.Fix_Contexts();
      if (op_status != 0)
	op_status = 1;		// failure
    }
  operation_end(op_status);
  return 0;
}

int GUIAction::fixpermissions(std::string arg)
{
  return fixcontexts(arg);
}

int GUIAction::dd(std::string arg)
{
  operation_start("imaging");

  if (simulate)
    {
      simulate_progress_bar();
    }
  else
    {
      string cmd = "dd " + arg;
      TWFunc::Exec_Cmd(cmd);
    }
  operation_end(0);
  return 0;
}

int GUIAction::partitionsd(std::string arg __unused)
{
  operation_start("Partition SD Card");
  int ret_val = 0;

  if (simulate)
    {
      TWPartition *SDCard = PartitionManager.Find_Partition_By_Path(DataManager::GetCurrentPartPath());
      LOGINFO("DEBUG: Selected partition: %s\n", DataManager::GetCurrentPartPath().c_str());
      simulate_progress_bar();
    }
  else
    {
      int allow_partition;
      DataManager::GetValue(TW_ALLOW_PARTITION_SDCARD, allow_partition);
      if (allow_partition == 0)
	{
	  gui_err
	    ("no_real_sdcard=This device does not have a real SD Card! Aborting!");
	}
      else
	{
	  if (!PartitionManager.Partition_SDCard())
	    ret_val = 1;	// failed
	}
    }
  operation_end(ret_val);
  return 0;

}

int GUIAction::installhtcdumlock(std::string arg __unused)
{
  operation_start("Install HTC Dumlock");
  if (simulate)
    {
      simulate_progress_bar();
    }
  else
    TWFunc::install_htc_dumlock();

  operation_end(0);
  return 0;
}

int GUIAction::htcdumlockrestoreboot(std::string arg __unused)
{
  operation_start("HTC Dumlock Restore Boot");
  if (simulate)
    {
      simulate_progress_bar();
    }
  else
    TWFunc::htc_dumlock_restore_original_boot();

  operation_end(0);
  return 0;
}

int GUIAction::htcdumlockreflashrecovery(std::string arg __unused)
{
  operation_start("HTC Dumlock Reflash Recovery");
  if (simulate)
    {
      simulate_progress_bar();
    }
  else
    TWFunc::htc_dumlock_reflash_recovery_to_boot();

  operation_end(0);
  return 0;
}

int GUIAction::cmd(std::string arg)
{
  int op_status = 0;

  operation_start("Command");
  LOGINFO("Running command: '%s'\n", arg.c_str());
  if (simulate)
    {
      simulate_progress_bar();
    }
  else
    {
      op_status = TWFunc::Exec_Cmd(arg);
      if (op_status != 0)
	op_status = 1;
    }

  operation_end(op_status);
  return 0;
}

int GUIAction::ftls(std::string arg)
{
  int op_status = 0;
  DataManager::SetValue("ftls_running", "1");

  op_status = TWFunc::Exec_Cmd(arg, false);

  DataManager::SetValue("ftls_errcode", op_status);
  DataManager::SetValue("ftls_running", "0");
  return 0;
}

int GUIAction::terminalcommand(std::string arg)
{
  int op_status = 0;
  string cmdpath, command;

  DataManager::GetValue("tw_terminal_location", cmdpath);
  operation_start("CommandOutput");
  //[f/d] Silent terminal
  //gui_print("%s # %s\n", cmdpath.c_str(), arg.c_str());
  if (simulate)
    {
      simulate_progress_bar();
      operation_end(op_status);
    }
  else if (arg == "exit")
    {
      LOGINFO("Exiting terminal\n");
      operation_end(op_status);
      page("main");
    }
  else
    {
      command = "cd \"" + cmdpath + "\" && " + arg + " 2>&1";;
      LOGINFO("Actual command is: '%s'\n", command.c_str());
      DataManager::SetValue("tw_terminal_state", 1);
      DataManager::SetValue("tw_background_thread_running", 1);
      FILE *fp;
      char line[512];

      fp = popen(command.c_str(), "r");
      if (fp == NULL)
	{
	  LOGERR("Error opening command to run (%s).\n", strerror(errno));
	}
      else
	{
	  int fd = fileno(fp), has_data = 0, check = 0, keep_going = -1;
	  struct timeval timeout;
	  fd_set fdset;

	  while (keep_going)
	    {
	      FD_ZERO(&fdset);
	      FD_SET(fd, &fdset);
	      timeout.tv_sec = 0;
	      timeout.tv_usec = 400000;
	      has_data = select(fd + 1, &fdset, NULL, NULL, &timeout);
	      if (has_data == 0)
		{
		  // Timeout reached
		  DataManager::GetValue("tw_terminal_state", check);
		  if (check == 0)
		    {
		      keep_going = 0;
		    }
		}
	      else if (has_data < 0)
		{
		  // End of execution
		  keep_going = 0;
		}
	      else
		{
		  // Try to read output
		  if (fgets(line, sizeof(line), fp) != NULL)
		    gui_print("%s", line);	// Display output
		  else
		    keep_going = 0;	// Done executing
		}
	    }
	  fclose(fp);
	}
      DataManager::SetValue("tw_operation_status", 0); //WEXITSTATUS(pclose(fp)) != 0 ? 1 : 0
      DataManager::SetValue("tw_operation_state", 1);
      DataManager::SetValue("tw_terminal_state", 0);
      DataManager::SetValue("tw_background_thread_running", 0);
      DataManager::SetValue(TW_ACTION_BUSY, 0);
    }
  return 0;
}

int GUIAction::killterminal(std::string arg __unused)
{
  LOGINFO("Sending kill command...\n");
  operation_start("KillCommand");
  DataManager::SetValue("tw_operation_status", 0);
  DataManager::SetValue("tw_operation_state", 1);
  DataManager::SetValue("tw_terminal_state", 0);
  DataManager::SetValue("tw_background_thread_running", 0);
  DataManager::SetValue(TW_ACTION_BUSY, 0);
  return 0;
}

int GUIAction::reinjecttwrp(std::string arg __unused)
{
  int op_status = 0;
  operation_start("ReinjectTWRP");
  gui_msg("injecttwrp=Injecting TWRP into boot image...");
  if (simulate)
    {
      simulate_progress_bar();
    }
  else
    {
      TWFunc::
	Exec_Cmd
	("injecttwrp --dump /tmp/backup_recovery_ramdisk.img /tmp/injected_boot.img --flash");
      gui_msg("done=Done.");
    }

  operation_end(op_status);
  return 0;
}

int GUIAction::checkbackupname(std::string arg __unused)
{
	int op_status = 0;

	operation_start("CheckBackupName");
	if (simulate) {
		simulate_progress_bar();
	} else {
		string Backup_Name;
		DataManager::GetValue(TW_BACKUP_NAME, Backup_Name);
		op_status = PartitionManager.Check_Backup_Name(Backup_Name, true, true);
		if (op_status != 0)
			op_status = 1;
	}

	operation_end(op_status);
	return 0;
}


int GUIAction::decrypt(std::string arg __unused)
{
  int op_status = 0;

  operation_start("Decrypt");
  if (simulate)
    {
      simulate_progress_bar();
    }
  else
    {
      string Password;
      DataManager::GetValue("tw_crypto_password", Password);
      op_status = PartitionManager.Decrypt_Device(Password);
      if (op_status != 0)
	op_status = 1;
      else
	{

	  DataManager::SetValue(TW_IS_ENCRYPTED, 0);

	  int has_datamedia;

	  // Check for a custom theme and load it if exists
	  DataManager::GetValue(TW_HAS_DATA_MEDIA, has_datamedia);
	  if (has_datamedia != 0)
	    {
	      if (tw_get_default_metadata
		  (DataManager::GetSettingsStoragePath().c_str()) != 0)
		{
		  LOGINFO
		    ("Failed to get default contexts and file mode for storage files.\n");
		}
	      else
		{
		  LOGINFO
		    ("Got default contexts and file mode for storage files.\n");
		}
	    }
	  PartitionManager.Decrypt_Adopted();
	}
    }

  operation_end(op_status);
  return 0;
}

int GUIAction::adbsideload(std::string arg __unused)
{
	operation_start("Sideload");
	if (simulate) {
		simulate_progress_bar();
		operation_end(0);
	} else {
		gui_msg("start_sideload=Starting ADB sideload feature...");
		bool mtp_was_enabled = TWFunc::Toggle_MTP(false);

		// wait for the adb connection
		Device::BuiltinAction reboot_action = Device::REBOOT_BOOTLOADER;
		int ret = twrp_sideload("/", &reboot_action);
		sideload_child_pid = GetMiniAdbdPid();
		DataManager::SetValue("tw_has_cancel", 0); // Remove cancel button from gui now that the zip install is going to start

		if (ret != 0) {
			if (ret == -2)
				gui_msg("need_new_adb=You need adb 1.0.32 or newer to sideload to this device.");
			ret = 1; // failure
		} else {
			int wipe_cache = 0;
			int wipe_dalvik = 0;
			DataManager::GetValue("tw_wipe_dalvik", wipe_dalvik);
			if (wipe_cache || DataManager::GetIntValue("tw_wipe_cache"))
				PartitionManager.Wipe_By_Path("/cache");
			if (wipe_dalvik)
				PartitionManager.Wipe_Dalvik_Cache();
		}
		TWFunc::Toggle_MTP(mtp_was_enabled);
		reinject_after_flash();
		operation_end(ret);
	}
	return 0;
}

int GUIAction::adbsideloadcancel(std::string arg __unused)
{
	struct stat st;
	DataManager::SetValue("tw_has_cancel", 0); // Remove cancel button from gui
	gui_msg("cancel_sideload=Cancelling ADB sideload...");
	LOGINFO("Signaling child sideload process to exit.\n");
	// Calling stat() on this magic filename signals the minadbd
	// subprocess to shut down.
	stat(FUSE_SIDELOAD_HOST_EXIT_PATHNAME, &st);
	sideload_child_pid = GetMiniAdbdPid();
	if (!sideload_child_pid) {
		LOGERR("Unable to get child ID\n");
		return 0;
	}
	::sleep(1);
	LOGINFO("Killing child sideload process.\n");
	kill(sideload_child_pid, SIGTERM);
	int status;
	LOGINFO("Waiting for child sideload process to exit.\n");
	waitpid(sideload_child_pid, &status, 0);
	sideload_child_pid = 0;
	DataManager::SetValue("tw_page_done", "1"); // For OpenRecoveryScript support
	return 0;
}

int GUIAction::openrecoveryscript(std::string arg __unused)
{
  operation_start("OpenRecoveryScript");
  if (simulate)
    {
      simulate_progress_bar();
      operation_end(0);
    }
  else
    {
      int op_status = OpenRecoveryScript::Run_OpenRecoveryScript_Action();
      operation_end(op_status);
    }
  return 0;
}

int GUIAction::installsu(std::string arg __unused)
{
	int op_status = 0;

	operation_start("Install SuperSU");
	if (simulate) {
		simulate_progress_bar();
	} else {
		LOGERR("Installing SuperSU was deprecated from TWRP.\n");
	}

	operation_end(op_status);
	return 0;
}

int GUIAction::fixsu(std::string arg __unused)
{
	int op_status = 0;

	operation_start("Fixing Superuser Permissions");
	if (simulate) {
		simulate_progress_bar();
	} else {
		LOGERR("Fixing su permissions was deprecated from TWRP.\n");
		LOGERR("4.3+ ROMs with SELinux will always lose su perms.\n");
	}

	operation_end(op_status);
	return 0;
}

int GUIAction::decrypt_backup(std::string arg __unused)
{
  int op_status = 0;

  operation_start("Try Restore Decrypt");
  if (simulate)
    {
      simulate_progress_bar();
    }
  else
    {
      string Restore_Path, Filename, Password;
      DataManager::GetValue("tw_restore", Restore_Path);
      Restore_Path += "/";
      DataManager::GetValue("tw_restore_password", Password);
      TWFunc::SetPerformanceMode(true);
      if (TWFunc::Try_Decrypting_Backup(Restore_Path, Password))
	op_status = 0;		// success
      else
	op_status = 1;		// fail
      TWFunc::SetPerformanceMode(false);
    }

  operation_end(op_status);
  return 0;
}

int GUIAction::repair(std::string arg __unused)
{
  int op_status = 0;

  operation_start("Repair Partition");
  if (simulate)
    {
      simulate_progress_bar();
    }
  else
    {
      string part_path;
      DataManager::GetValue("tw_partition_mount_point", part_path);
      if (PartitionManager.Repair_By_Path(part_path, true))
	{
	  op_status = 0;	// success
	}
      else
	{
	  op_status = 1;	// fail
	}
    }

  operation_end(op_status);
  return 0;
}

int GUIAction::resize(std::string arg __unused)
{
  int op_status = 0;

  operation_start("Resize Partition");
  if (simulate)
    {
      simulate_progress_bar();
    }
  else
    {
      string part_path;
      DataManager::GetValue("tw_partition_mount_point", part_path);
      if (PartitionManager.Resize_By_Path(part_path, true))
	{
	  op_status = 0;	// success
	}
      else
	{
	  op_status = 1;	// fail
	}
    }

  operation_end(op_status);
  return 0;
}

int GUIAction::changefilesystem(std::string arg __unused)
{
  int op_status = 0;

  operation_start("Change File System");
  if (simulate)
    {
      simulate_progress_bar();
    }
  else
    {
      string part_path, file_system;
      DataManager::GetValue("tw_partition_mount_point", part_path);
      DataManager::GetValue("tw_action_new_file_system", file_system);
      if (PartitionManager.Wipe_By_Path(part_path, file_system))
	{
	  op_status = 0;	// success
	}
      else
	{
	  gui_err("change_fs_err=Error changing file system.");
	  op_status = 1;	// fail
	}
    }
  PartitionManager.Update_System_Details();
  operation_end(op_status);
  return 0;
}

int GUIAction::startmtp(std::string arg __unused)
{
  int op_status = 0;

  operation_start("Start MTP");
  if (PartitionManager.Enable_MTP())
    op_status = 0;		// success
  else
    op_status = 1;		// fail

  operation_end(op_status);
  return 0;
}

int GUIAction::stopmtp(std::string arg __unused)
{
  int op_status = 0;

  operation_start("Stop MTP");
  if (PartitionManager.Disable_MTP())
    op_status = 0;		// success
  else
    op_status = 1;		// fail

  operation_end(op_status);
  return 0;
}

int GUIAction::flashimage(std::string arg __unused)
{
  int op_status = 0;

  operation_start("Flash Image");
  string path, filename;
  DataManager::GetValue("tw_zip_location", path);
  DataManager::GetValue("tw_file", filename);
  if (PartitionManager.Flash_Image(path, filename))
    op_status = 0;		// success
  else
    op_status = 1;		// fail

  DataManager::Leds(true);
  operation_end(op_status);
  return 0;
}

int GUIAction::twcmd(std::string arg)
{
  operation_start("TWRP CLI Command");
  if (simulate)
    simulate_progress_bar();
  else
    OpenRecoveryScript::Run_CLI_Command(arg.c_str());
  operation_end(0);
  return 0;
}

int GUIAction::getKeyByName(std::string key)
{
  if (key == "home")
    return KEY_HOMEPAGE;	// note: KEY_HOME is cursor movement (like KEY_END)
  else if (key == "menu")
    return KEY_MENU;
  else if (key == "back")
    return KEY_BACK;
  else if (key == "search")
    return KEY_SEARCH;
  else if (key == "voldown")
    return KEY_VOLUMEDOWN;
  else if (key == "volup")
    return KEY_VOLUMEUP;
  else if (key == "power")
    {
      int ret_val;
      DataManager::GetValue(TW_POWER_BUTTON, ret_val);
      if (!ret_val)
	return KEY_POWER;
      else
	return ret_val;
    }

  return atol(key.c_str());
}

int GUIAction::checkpartitionlifetimewrites(std::string arg)
{
  int op_status = 0;
  TWPartition *sys = PartitionManager.Find_Partition_By_Path(arg);

  operation_start("Check Partition Lifetime Writes");
  if (sys)
    {
      if (sys->Check_Lifetime_Writes() != 0)
	DataManager::SetValue("tw_lifetime_writes", 1);
      else
	DataManager::SetValue("tw_lifetime_writes", 0);
      op_status = 0;		// success
    }
  else
    {
      DataManager::SetValue("tw_lifetime_writes", 1);
      op_status = 1;		// fail
    }

  operation_end(op_status);
  return 0;
}

int GUIAction::mountsystemtoggle(std::string arg)
{
	int op_status = 0;
	bool remount_system = PartitionManager.Is_Mounted_By_Path(PartitionManager.Get_Android_Root_Path());
	bool remount_vendor = PartitionManager.Is_Mounted_By_Path("/vendor");

	operation_start("Toggle System Mount");
	if (!PartitionManager.UnMount_By_Path(PartitionManager.Get_Android_Root_Path(), true)) {
		op_status = 1; // fail
	} else {
		TWPartition* Part = PartitionManager.Find_Partition_By_Path(PartitionManager.Get_Android_Root_Path());
		if (Part) {
			if (arg == "0") {
				DataManager::SetValue("tw_mount_system_ro", 0);
				Part->Change_Mount_Read_Only(false);
			} else {
				DataManager::SetValue("tw_mount_system_ro", 1);
				Part->Change_Mount_Read_Only(true);
			}
			if (remount_system) {
				Part->Mount(true);
			}
			op_status = 0; // success
		} else {
			op_status = 1; // fail
		}
		Part = PartitionManager.Find_Partition_By_Path("/vendor");
		if (Part) {
			if (arg == "0") {
				Part->Change_Mount_Read_Only(false);
			} else {
				Part->Change_Mount_Read_Only(true);
			}
			if (remount_vendor) {
				Part->Mount(true);
			}
			op_status = 0; // success
		} else {
			op_status = 1; // fail
		}
	}

	operation_end(op_status);
	return 0;
}

int GUIAction::setlanguage(std::string arg __unused)
{
  int op_status = 0;

  operation_start("Set Language");
  PageManager::LoadLanguage(DataManager::GetStrValue("tw_language"));
  PageManager::RequestReload();
  op_status = 0;		// success

  operation_end(op_status);
  return 0;
}

int GUIAction::togglebacklight(std::string arg __unused)
{
  blankTimer.toggleBlank();
  return 0;
}

int GUIAction::setbootslot(std::string arg)
{
	operation_start("Set Boot Slot");
	if (!simulate) {
		if (!PartitionManager.UnMount_By_Path("/vendor", false)) {
			// PartitionManager failed to unmount /vendor, this should not happen,
			// but in case it does, do a lazy unmount
			LOGINFO("WARNING: vendor partition could not be unmounted normally!\n");
			umount2("/vendor", MNT_DETACH);
			PartitionManager.Set_Active_Slot(arg);
		} else {
			PartitionManager.Set_Active_Slot(arg);
		}
	} else {
		simulate_progress_bar();
	}
	operation_end(0);
	return 0;
}

int GUIAction::flashlight(std::string arg __unused)
{
    string enable_flash;
    DataManager::GetValue(OF_FLASHLIGHT_ENABLE_STR, enable_flash);
    if (enable_flash == "1") {
		std::string path_one, path_two,
					fl_one_on, fl_two_on,
					max_one, max_two,
					bright_one, bright_two,
					max_brt_one, max_brt_two,
					fl_used;
		
		// get maintainer flash files
		DataManager::GetValue("of_fl_path_1", path_one);
		DataManager::GetValue("of_fl_path_2", path_two);
		DataManager::GetValue("of_flash_on", fl_used);
		
		if (path_one.empty() && path_two.empty()) {
			// maintainer not set flash paths
			if (TWFunc::Path_Exists("/sys/class/leds/flashlight/brightness")) {
				// use flashlight for old devices
				path_one = "/sys/class/leds/flashlight";
			} else {
				// use flashlight for new devices
				path_one = "/sys/class/leds/led:torch_0";
				path_two = "/sys/class/leds/led:switch_0";
			}
		} else if (path_one.empty() && !path_two.empty()) {
			path_one = path_two;
			path_two = "";
		}
		
		if (!path_one.empty()) {
			bright_one = path_one + "/brightness";
			max_one = path_one + "/max_brightness";
			if (TWFunc::Path_Exists(max_one)) {
				TWFunc::read_file(max_one, max_brt_one);
			} else {
				max_brt_one = "1";
			}
			if (TWFunc::Path_Exists(bright_one)) {
				TWFunc::read_file(bright_one, fl_one_on);
				// If we use flashlight first time after reboot, always enable it
				if (fl_one_on == "0" || fl_used == "0") {
					TWFunc::write_to_file(bright_one, max_brt_one);
          DataManager::SetValue("of_flash_on", "1");
				} else {
					TWFunc::write_to_file(bright_one, "0");
          DataManager::SetValue("of_flash_on", "0");
				}
			} else {
				gui_print_color("warning", "Flashlight file not found!\n");
				return 0;
			}
		}
		
		if (!path_two.empty()) {
			bright_two = path_two + "/brightness";
			max_two = path_two + "/max_brightness";
			if (TWFunc::Path_Exists(bright_two)) {
				if (!TWFunc::Path_Exists(max_two)) {
					max_brt_two = "1";
				} else {
					TWFunc::read_file(max_two, max_brt_two);
				}
				TWFunc::read_file(bright_two, fl_two_on);
				if ((fl_two_on == "0" && fl_one_on == "0") || fl_used == "0") {
					TWFunc::write_to_file(bright_two, max_brt_two);
          DataManager::SetValue("of_flash_on", "1");
				} else {
					TWFunc::write_to_file(bright_two, "0");
          DataManager::SetValue("of_flash_on", "0");
				}
			}
		}
  }
  return 0;
}

int GUIAction::calldeactivateprocess(std::string arg __unused)
{
  operation_start("Patch DM-Verity and Forced-Encryption");
  if (simulate)
    {
      	simulate_progress_bar();
    }
  else
    {
  	DataManager::SetValue(FOX_FORCE_DEACTIVATE_PROCESS, 1);
  	usleep(1024);
  	DataManager::GetValue(FOX_FORCE_DEACTIVATE_PROCESS, Fox_Force_Deactivate_Process);
  	TWFunc::Deactivation_Process();
    }
  operation_end(0);
  return 0;
}

int GUIAction::disable_replace(std::string arg __unused)
{
  operation_start("Disable stocker recovery's replace");
  if (simulate)
    {
      	simulate_progress_bar();
    }
  else
  {
    TWFunc::Disable_Stock_Recovery_Replace();
  }
  operation_end(0);
  return 0;
}

int GUIAction::disableled(std::string arg __unused)
{
  DataManager::Leds(false);
  return 0;
}

int GUIAction::wlfw(std::string arg __unused)
{
  operation_start("WLFW");
  if (simulate)
    {
      simulate_progress_bar();
    }
  else
    {
      TWFunc::Unpack_Image("/recovery");
    }
  operation_end(0);
  return 0;
}

int GUIAction::wlfx(std::string arg __unused)
{
  operation_start("WLFX");
  if (simulate)
    {
      simulate_progress_bar();
    }
  else
    {
      DataManager::Flush();
      TWFunc::Repack_Image("/recovery");
    }
  operation_end(0);
  return 0;
}

int GUIAction::adb(std::string arg)
{
  operation_start("ADB");
  if (simulate)
    {
      simulate_progress_bar();
    }
  else
    {
      if (arg == "enable")
      {
        property_set("ctl.start", "adbd");
        property_set("orangefox.adb.status", "1");
        DataManager::SetValue("fox_adb", "1");
      }
      if (arg == "disable")
      {
        property_set("ctl.stop", "adbd");
        property_set("orangefox.adb.status", "0");
        DataManager::SetValue("fox_adb", "0");
      }
    }
  operation_end(0);
  return 0;
}

int GUIAction::repackimage(std::string arg __unused)
{
	int op_status = 1;
	twrpRepacker repacker;

	operation_start("Repack Image");
	if (!simulate)
	{
		std::string path = DataManager::GetStrValue("tw_filename");
		Repack_Options_struct Repack_Options;
		Repack_Options.Disable_Verity = false;
		Repack_Options.Disable_Force_Encrypt = false;
		Repack_Options.Backup_First = DataManager::GetIntValue("tw_repack_backup_first") != 0;
		if (DataManager::GetIntValue("tw_repack_kernel") == 1)
			Repack_Options.Type = REPLACE_KERNEL;
		else
			Repack_Options.Type = REPLACE_RAMDISK;
		if (!repacker.Repack_Image_And_Flash(path, Repack_Options))
			goto exit;
	} else
		simulate_progress_bar();
	op_status = 0;
exit:
	operation_end(op_status);
	return 0;
}

int GUIAction::fixabrecoverybootloop(std::string arg __unused)
{
	int op_status = 1;
	twrpRepacker repacker;
	std::string magiskboot = TWFunc::Get_MagiskBoot();
	operation_start("Repack Image");
	if (!simulate)
	{
		if (!TWFunc::Path_Exists(magiskboot)) {
			LOGERR("Image repacking tool not present in this TWRP build!");
			goto exit;
		}
		DataManager::SetProgress(0);
		TWPartition* part = PartitionManager.Find_Partition_By_Path("/boot");
		if (part)
			gui_msg(Msg("unpacking_image=Unpacking {1}...")(part->Display_Name));
		else {
			gui_msg(Msg(msg::kError, "unable_to_locate=Unable to locate {1}.")("/boot"));
			goto exit;
		}
		if (!repacker.Backup_Image_For_Repack(part, REPACK_ORIG_DIR, DataManager::GetIntValue("tw_repack_backup_first") != 0, gui_lookup("repack", "Repack")))
			goto exit;
		DataManager::SetProgress(.25);
		gui_msg("fixing_recovery_loop_patch=Patching kernel...");
		std::string command = "cd " REPACK_ORIG_DIR " && " + magiskboot + " hexpatch kernel 77616E745F696E697472616D667300 736B69705F696E697472616D667300";
		if (TWFunc::Exec_Cmd(command) != 0) {
			gui_msg(Msg(msg::kError, "fix_recovery_loop_patch_error=Error patching kernel."));
			goto exit;
		}
		std::string header_path = REPACK_ORIG_DIR;
		header_path += "header";
		if (TWFunc::Path_Exists(header_path)) {
			command = "cd " REPACK_ORIG_DIR " && sed -i \"s|$(grep '^cmdline=' header | cut -d= -f2-)|$(grep '^cmdline=' header | cut -d= -f2- | sed -e 's/skip_override//' -e 's/  */ /g' -e 's/[ \t]*$//')|\" header";
			if (TWFunc::Exec_Cmd(command) != 0) {
				gui_msg(Msg(msg::kError, "fix_recovery_loop_patch_error=Error patching kernel."));
				goto exit;
			}
		}
		DataManager::SetProgress(.5);
		gui_msg(Msg("repacking_image=Repacking {1}...")(part->Display_Name));
		command = "cd " REPACK_ORIG_DIR " && " + magiskboot + " repack " REPACK_ORIG_DIR "boot.img";
		if (TWFunc::Exec_Cmd(command) != 0) {
			gui_msg(Msg(msg::kError, "repack_error=Error repacking image."));
			goto exit;
		}
		DataManager::SetProgress(.75);
		std::string path = REPACK_ORIG_DIR;
		std::string file = "new-boot.img";
		DataManager::SetValue("tw_flash_partition", "/boot;");
		if (!PartitionManager.Flash_Image(path, file)) {
			LOGINFO("Error flashing new image\n");
			goto exit;
		}
		DataManager::SetProgress(1);
		TWFunc::removeDir(REPACK_ORIG_DIR, false);
	} else
		simulate_progress_bar();
	op_status = 0;
exit:
	operation_end(op_status);
	return 0;
}

int GUIAction::cmdf(std::string arg, std::string file)
{
  char buff[1024];
  sprintf(buff, gui_parse_text(arg).c_str(), file.c_str());

  gui_print( std::string(DataManager::GetStrValue("tw_fm_text1") + ": %s").c_str() , file.c_str());

   int op_status = 0;
  string cmdpath, command;

  //begin terminal
  DataManager::GetValue("tw_terminal_location", cmdpath);
  command = "cd \"" + cmdpath + "\" && " + std::string(buff) + " 2>&1";
  LOGINFO("Actual command is: '%s'\n", command.c_str());
  DataManager::SetValue("tw_terminal_state", 1);
  DataManager::SetValue("tw_background_thread_running", 1);
  FILE* fp;
  char line[512];

  fp = popen(command.c_str(), "r");
  if (fp == NULL) {
    LOGERR("Error opening command to run (%s).\n", strerror(errno));
    return 1;
  }

  int fd = fileno(fp), has_data = 0, check = 0, keep_going = -1;
  struct timeval timeout;
  fd_set fdset;

  while (keep_going) {
    FD_ZERO(&fdset);
    FD_SET(fd, &fdset);
    timeout.tv_sec = 0;
    timeout.tv_usec = 400000;
    has_data = select(fd + 1, &fdset, NULL, NULL, &timeout);
    if (has_data == 0) {
      DataManager::GetValue("tw_terminal_state", check);
      if (check == 0) {
        keep_going = 0;
      }
    } else if (has_data < 0) {
      keep_going = 0;
    } else {
      if (fgets(line, sizeof(line), fp) != NULL)
        gui_print("%s", line);
      else
        keep_going = 0;
    }
  }
  fclose(fp);
  DataManager::SetValue("tw_terminal_state", 0);
  DataManager::SetValue("tw_background_thread_running", 0);
  return 0; //WEXITSTATUS(pclose(fp)) != 0 ? 1 : 0
}

int GUIAction::batch(std::string arg __unused)
{
  operation_start("BatchCommandOutput");
  int op_status = 0;
  std::string list, cmd;

  if (simulate) {
    simulate_progress_bar();
    operation_end(op_status);
  } else {

    DataManager::GetValue("of_batch_files", list);
    DataManager::GetValue("of_batch_files_cmd", cmd);
  
    for (int i = 0; i <= 1; i++) {
      LOGINFO("Process list: %s\n", list.c_str());
      if (!cmd.empty() && !list.empty()) {
        list = list.substr(0, list.size()-1); //remove last delimiter ("/")
        std::string delimiter = "/";

        size_t pos = 0;
        std::string token;
        while ((pos = list.find(delimiter)) != std::string::npos) {
            token = list.substr(0, pos);
            op_status = cmdf(cmd, token);
            if (op_status == 1) break;
            list.erase(0, pos + delimiter.length());
        }
        if (op_status == 0) cmdf(cmd, list);
      }

      //repeat code with new vars
      DataManager::GetValue("of_batch_folders", list);
      DataManager::GetValue("of_batch_folders_cmd", cmd);
    }

  }
  
  DataManager::SetValue("tw_operation_status", op_status);
  DataManager::SetValue("tw_operation_state", 1);
  DataManager::SetValue(TW_ACTION_BUSY, 0);
  
  return 0;
}

int GUIAction::enableadb(std::string arg __unused) {
	android::base::SetProperty("sys.usb.config", "none");
	android::base::SetProperty("sys.usb.config", "adb");
	return 0;
}

int GUIAction::enablefastboot(std::string arg __unused) {
	android::base::SetProperty("sys.usb.config", "none");
	android::base::SetProperty("sys.usb.config", "fastboot");
	return 0;
}
