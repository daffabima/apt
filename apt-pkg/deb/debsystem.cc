// -*- mode: cpp; mode: fold -*-
// Description								/*{{{*/
// $Id: debsystem.cc,v 1.4 2004/01/26 17:01:53 mdz Exp $
/* ######################################################################

   System - Abstraction for running on different systems.

   Basic general structure..
   
   ##################################################################### */
									/*}}}*/
// Include Files							/*{{{*/
#include <config.h>

#include <apt-pkg/debsystem.h>
#include <apt-pkg/debversion.h>
#include <apt-pkg/debindexfile.h>
#include <apt-pkg/dpkgpm.h>
#include <apt-pkg/configuration.h>
#include <apt-pkg/error.h>
#include <apt-pkg/fileutl.h>
#include <apt-pkg/pkgcache.h>
#include <apt-pkg/cacheiterators.h>

#include <algorithm>

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>

#include <apti18n.h>
									/*}}}*/

using std::string;

debSystem debSys;

class APT_HIDDEN debSystemPrivate {
public:
   debSystemPrivate() : LockFD(-1), LockCount(0), StatusFile(0)
   {
   }
   // For locking support
   int LockFD;
   unsigned LockCount;
   
   debStatusIndex *StatusFile;
};

// System::debSystem - Constructor					/*{{{*/
// ---------------------------------------------------------------------
/* */
debSystem::debSystem() : pkgSystem("Debian dpkg interface", &debVS), d(new debSystemPrivate())
{
}
									/*}}}*/
// System::~debSystem - Destructor					/*{{{*/
// ---------------------------------------------------------------------
/* */
debSystem::~debSystem()
{
   delete d->StatusFile;
   delete d;
}
									/*}}}*/
// System::Lock - Get the lock						/*{{{*/
// ---------------------------------------------------------------------
/* This mirrors the operations dpkg does when it starts up. Note the
   checking of the updates directory. */
bool debSystem::Lock()
{
   // Disable file locking
   if (_config->FindB("Debug::NoLocking",false) == true || d->LockCount > 1)
   {
      d->LockCount++;
      return true;
   }

   // Create the lockfile
   string AdminDir = flNotFile(_config->Find("Dir::State::status"));
   d->LockFD = GetLock(AdminDir + "lock");
   if (d->LockFD == -1)
   {
      if (errno == EACCES || errno == EAGAIN)
	 return _error->Error(_("Unable to lock the administration directory (%s), "
	                        "is another process using it?"),AdminDir.c_str());
      else
	 return _error->Error(_("Unable to lock the administration directory (%s), "
	                        "are you root?"),AdminDir.c_str());
   }
   
   // See if we need to abort with a dirty journal
   if (CheckUpdates() == true)
   {
      close(d->LockFD);
      d->LockFD = -1;
      const char *cmd;
      if (getenv("SUDO_USER") != NULL)
	 cmd = "sudo dpkg --configure -a";
      else
	 cmd = "dpkg --configure -a";
      // TRANSLATORS: the %s contains the recovery command, usually
      //              dpkg --configure -a
      return _error->Error(_("dpkg was interrupted, you must manually "
                             "run '%s' to correct the problem. "), cmd);
   }

	 d->LockCount++;
      
   return true;
}
									/*}}}*/
// System::UnLock - Drop a lock						/*{{{*/
// ---------------------------------------------------------------------
/* */
bool debSystem::UnLock(bool NoErrors)
{
   if (d->LockCount == 0 && NoErrors == true)
      return false;
   
   if (d->LockCount < 1)
      return _error->Error(_("Not locked"));
   if (--d->LockCount == 0)
   {
      close(d->LockFD);
      d->LockCount = 0;
   }
   
   return true;
}
									/*}}}*/
// System::CheckUpdates - Check if the updates dir is dirty		/*{{{*/
// ---------------------------------------------------------------------
/* This does a check of the updates directory (dpkg journal) to see if it has 
   any entries in it. */
bool debSystem::CheckUpdates()
{
   // Check for updates.. (dirty)
   string File = flNotFile(_config->Find("Dir::State::status")) + "updates/";
   DIR *DirP = opendir(File.c_str());
   if (DirP == 0)
      return false;
   
   /* We ignore any files that are not all digits, this skips .,.. and 
      some tmp files dpkg will leave behind.. */
   bool Damaged = false;
   for (struct dirent *Ent = readdir(DirP); Ent != 0; Ent = readdir(DirP))
   {
      Damaged = true;
      for (unsigned int I = 0; Ent->d_name[I] != 0; I++)
      {
	 // Check if its not a digit..
	 if (isdigit(Ent->d_name[I]) == 0)
	 {
	    Damaged = false;
	    break;
	 }
      }
      if (Damaged == true)
	 break;
   }
   closedir(DirP);

   return Damaged;
}
									/*}}}*/
// System::CreatePM - Create the underlying package manager		/*{{{*/
// ---------------------------------------------------------------------
/* */
pkgPackageManager *debSystem::CreatePM(pkgDepCache *Cache) const
{
   return new pkgDPkgPM(Cache);
}
									/*}}}*/
// System::Initialize - Setup the configuration space..			/*{{{*/
// ---------------------------------------------------------------------
/* These are the Debian specific configuration variables.. */
bool debSystem::Initialize(Configuration &Cnf)
{
   /* These really should be jammed into a generic 'Local Database' engine
      which is yet to be determined. The functions in pkgcachegen should
      be the only users of these */
   Cnf.CndSet("Dir::State::extended_states", "extended_states");
   Cnf.CndSet("Dir::State::status","/var/lib/dpkg/status");
   Cnf.CndSet("Dir::Bin::dpkg","/usr/bin/dpkg");

   if (d->StatusFile) {
     delete d->StatusFile;
     d->StatusFile = 0;
   }

   return true;
}
									/*}}}*/
// System::ArchiveSupported - Is a file format supported		/*{{{*/
// ---------------------------------------------------------------------
/* The standard name for a deb is 'deb'.. There are no separate versions
   of .deb to worry about.. */
APT_PURE bool debSystem::ArchiveSupported(const char *Type)
{
   if (strcmp(Type,"deb") == 0)
      return true;
   return false;
}
									/*}}}*/
// System::Score - Determine how 'Debiany' this sys is..		/*{{{*/
// ---------------------------------------------------------------------
/* We check some files that are sure tell signs of this being a Debian
   System.. */
signed debSystem::Score(Configuration const &Cnf)
{
   signed Score = 0;
   if (FileExists(Cnf.FindFile("Dir::State::status","/var/lib/dpkg/status")) == true)
       Score += 10;
   if (FileExists(Cnf.FindFile("Dir::Bin::dpkg","/usr/bin/dpkg")) == true)
      Score += 10;
   if (FileExists("/etc/debian_version") == true)
      Score += 10;
   return Score;
}
									/*}}}*/
// System::AddStatusFiles - Register the status files			/*{{{*/
// ---------------------------------------------------------------------
/* */
bool debSystem::AddStatusFiles(std::vector<pkgIndexFile *> &List)
{
   if (d->StatusFile == 0)
      d->StatusFile = new debStatusIndex(_config->FindFile("Dir::State::status"));
   List.push_back(d->StatusFile);
   return true;
}
									/*}}}*/
// System::FindIndex - Get an index file for status files		/*{{{*/
// ---------------------------------------------------------------------
/* */
bool debSystem::FindIndex(pkgCache::PkgFileIterator File,
			  pkgIndexFile *&Found) const
{
   if (d->StatusFile == 0)
      return false;
   if (d->StatusFile->FindInCache(*File.Cache()) == File)
   {
      Found = d->StatusFile;
      return true;
   }
   
   return false;
}
									/*}}}*/

std::string debSystem::GetDpkgExecutable()				/*{{{*/
{
   string Tmp = _config->Find("Dir::Bin::dpkg","dpkg");
   string const dpkgChrootDir = _config->FindDir("DPkg::Chroot-Directory", "/");
   size_t dpkgChrootLen = dpkgChrootDir.length();
   if (dpkgChrootDir != "/" && Tmp.find(dpkgChrootDir) == 0)
   {
      if (dpkgChrootDir[dpkgChrootLen - 1] == '/')
         --dpkgChrootLen;
      Tmp = Tmp.substr(dpkgChrootLen);
   }
   return Tmp;
}
									/*}}}*/
std::vector<std::string> debSystem::GetDpkgBaseCommand()		/*{{{*/
{
   // Generate the base argument list for dpkg
   std::vector<std::string> Args = { GetDpkgExecutable() };
   // Stick in any custom dpkg options
   Configuration::Item const *Opts = _config->Tree("DPkg::Options");
   if (Opts != 0)
   {
      Opts = Opts->Child;
      for (; Opts != 0; Opts = Opts->Next)
      {
	 if (Opts->Value.empty() == true)
	    continue;
	 Args.push_back(Opts->Value);
      }
   }
   return Args;
}
									/*}}}*/
void debSystem::DpkgChrootDirectory()					/*{{{*/
{
   std::string const chrootDir = _config->FindDir("DPkg::Chroot-Directory");
   if (chrootDir == "/")
      return;
   std::cerr << "Chrooting into " << chrootDir << std::endl;
   if (chroot(chrootDir.c_str()) != 0)
      _exit(100);
   if (chdir("/") != 0)
      _exit(100);
}
									/*}}}*/
bool debSystem::SupportsMultiArch()					/*{{{*/
{
   std::vector<std::string> const Args = GetDpkgBaseCommand();
   std::vector<const char *> cArgs(Args.size(), NULL);
   std::transform(Args.begin(), Args.end(), cArgs.begin(), [](std::string const &s) { return s.c_str(); });

   // we need to detect if we can qualify packages with the architecture or not
   cArgs.push_back("--assert-multi-arch");
   cArgs.push_back(NULL);

   pid_t dpkgAssertMultiArch = ExecFork();
   if (dpkgAssertMultiArch == 0)
   {
      DpkgChrootDirectory();
      // redirect everything to the ultimate sink as we only need the exit-status
      int const nullfd = open("/dev/null", O_RDONLY);
      dup2(nullfd, STDIN_FILENO);
      dup2(nullfd, STDOUT_FILENO);
      dup2(nullfd, STDERR_FILENO);
      execvp(cArgs[0], (char**) &cArgs[0]);
      _error->WarningE("dpkgGo", "Can't detect if dpkg supports multi-arch!");
      _exit(2);
   }

   if (dpkgAssertMultiArch > 0)
   {
      int Status = 0;
      while (waitpid(dpkgAssertMultiArch, &Status, 0) != dpkgAssertMultiArch)
      {
	 if (errno == EINTR)
	    continue;
	 _error->WarningE("dpkgGo", _("Waited for %s but it wasn't there"), "dpkg --assert-multi-arch");
	 break;
      }
      if (WIFEXITED(Status) == true && WEXITSTATUS(Status) == 0)
	 return true;
   }
   return false;
}
									/*}}}*/
std::vector<std::string> debSystem::SupportedArchitectures()		/*{{{*/
{
   std::vector<std::string> const sArgs = GetDpkgBaseCommand();
   std::vector<const char *> Args(sArgs.size(), NULL);
   std::transform(sArgs.begin(), sArgs.end(), Args.begin(), [](std::string const &s) { return s.c_str(); });
   Args.push_back("--print-foreign-architectures");
   Args.push_back(NULL);

   std::vector<std::string> archs;
   string const arch = _config->Find("APT::Architecture");
   if (arch.empty() == false)
      archs.push_back(arch);

   int external[2] = {-1, -1};
   if (pipe(external) != 0)
   {
      _error->WarningE("getArchitecture", "Can't create IPC pipe for dpkg --print-foreign-architectures");
      return archs;
   }

   pid_t dpkgMultiArch = ExecFork();
   if (dpkgMultiArch == 0) {
      close(external[0]);
      int const nullfd = open("/dev/null", O_RDONLY);
      dup2(nullfd, STDIN_FILENO);
      dup2(external[1], STDOUT_FILENO);
      dup2(nullfd, STDERR_FILENO);
      DpkgChrootDirectory();
      execvp(Args[0], (char**) &Args[0]);
      _error->WarningE("getArchitecture", "Can't detect foreign architectures supported by dpkg!");
      _exit(100);
   }
   close(external[1]);

   FILE *dpkg = fdopen(external[0], "r");
   if(dpkg != NULL) {
      char* buf = NULL;
      size_t bufsize = 0;
      while (getline(&buf, &bufsize, dpkg) != -1)
      {
	 char* tok_saveptr;
	 char* arch = strtok_r(buf, " ", &tok_saveptr);
	 while (arch != NULL) {
	    for (; isspace(*arch) != 0; ++arch);
	    if (arch[0] != '\0') {
	       char const* archend = arch;
	       for (; isspace(*archend) == 0 && *archend != '\0'; ++archend);
	       string a(arch, (archend - arch));
	       if (std::find(archs.begin(), archs.end(), a) == archs.end())
		  archs.push_back(a);
	    }
	    arch = strtok_r(NULL, " ", &tok_saveptr);
	 }
      }
      free(buf);
      fclose(dpkg);
   }
   ExecWait(dpkgMultiArch, "dpkg --print-foreign-architectures", true);
   return archs;
}
									/*}}}*/
