// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0
// proteusimg.cpp -- OURS (Proteus, docs/phorepoc/proteus-p3.md): the image broker. The loader starts an ARM program as a
// cpuhandler_arm.exe process with the PROGRAM's identity and capabilities (core sf_lepoc.cpp ForeignCpuRedirect), so the handler
// usually lacks AllFiles -- and reading \sys\ needs it (KCapFsSysFileOpenRd). This server holds AllFiles and opens, read-only,
// exactly what the handler has to load: an ARM E32Image (CPU ARMv4/5/6) in \sys\bin\ or \sys\bin\arm\ of any drive, for a client
// running cpuhandler_arm.exe. The open file goes to the client as an RFile handle (RFile::TransferToClient), never its data.
// Transient: started by the first client, gone when its last session closes.
#include <e32base.h>
#include <f32file.h>
#include "proteusimg.h"

namespace
{
class CImgServer;

class CImgSession : public CSession2
	{
public:
	~CImgSession();
	void CreateL();
	void ServiceL(const RMessage2& aMessage);
private:
	TInt Open(const RMessage2& aMessage);
	CImgServer& Server() { return *(CImgServer*)const_cast<CServer2*>(CSession2::Server()); }
	};

class CImgServer : public CServer2
	{
public:
	CImgServer() : CServer2(EPriorityStandard, ESharableSessions) {}
	~CImgServer() { iFs.Close(); }
	void ConstructL()
		{
		User::LeaveIfError(iFs.Connect());
		User::LeaveIfError(iFs.ShareProtected());		// the handles go to other processes
		StartL(KProteusImgServer);
		}
	CSession2* NewSessionL(const TVersion&, const RMessage2&) const { return new (ELeave) CImgSession; }
	void AddSession() { ++iSessions; }
	void DropSession() { if (--iSessions == 0) CActiveScheduler::Stop(); }
	RFs iFs;
private:
	TInt iSessions;
	};

CImgSession::~CImgSession() { Server().DropSession(); }
void CImgSession::CreateL() { Server().AddSession(); }

void CImgSession::ServiceL(const RMessage2& aMessage)
	{
	if (aMessage.Function() != EProteusImgOpen)
		{
		aMessage.Complete(KErrNotSupported);
		return;
		}
	const TInt r = Open(aMessage);
	if (r != KErrNone)
		aMessage.Complete(r);		// on success TransferToClient completed it
	}

// (path, TPckgBuf<TInt> sub-session handle) -> the file server session handle
TInt CImgSession::Open(const RMessage2& aMessage)
	{
	RThread client;
	TInt r = aMessage.Client(client);
	if (r != KErrNone)
		return r;
	RProcess proc;
	r = client.Process(proc);
	client.Close();
	if (r != KErrNone)
		return r;
	const TFileName image = proc.FileName();
	proc.Close();
	if (TParsePtrC(image).NameAndExt().CompareF(_L("cpuhandler_arm.exe")) != 0)
		return KErrPermissionDenied;			// only the handler loads ARM programs
	TFileName path;
	r = aMessage.Read(0, path);
	if (r != KErrNone)
		return r;
	TParse p;
	r = Server().iFs.Parse(path, p);
	if (r != KErrNone)
		return r;
	if (p.Path().CompareF(_L("\\sys\\bin\\")) != 0 && p.Path().CompareF(_L("\\sys\\bin\\arm\\")) != 0)
		return KErrPermissionDenied;
	RFile f;
	r = f.Open(Server().iFs, p.FullName(), EFileRead | EFileShareReadersOnly);
	if (r != KErrNone)
		return r;
	TBuf8<124> h;					// E32ImageHeader up to iCpuIdentifier (offset 122)
	r = f.Read(0, h);
	const TUint cpu = h.Length() >= 124 ? (h[122] | (h[123] << 8)) : 0;
	if (r != KErrNone || h.Length() < 124 || h.Mid(16, 4) != _L8("EPOC") || cpu < 0x2000 || cpu > 0x2002)
		{
		f.Close();
		return r != KErrNone ? r : KErrPermissionDenied;	// only ARM E32Images (a native one the handler never needs)
		}
	r = f.TransferToClient(aMessage, 1);
	f.Close();
	return r;
	}

void RunL()
	{
	CActiveScheduler* s = new (ELeave) CActiveScheduler;
	CleanupStack::PushL(s);
	CActiveScheduler::Install(s);
	CImgServer* server = new (ELeave) CImgServer;
	CleanupStack::PushL(server);
	server->ConstructL();
	User::LeaveIfError(User::RenameThread(KProteusImgServer));
	RProcess::Rendezvous(KErrNone);
	CActiveScheduler::Start();
	CleanupStack::PopAndDestroy(2, s);
	}
}

GLDEF_C TInt E32Main()
	{
	CTrapCleanup* cleanup = CTrapCleanup::New();
	if (!cleanup)
		return KErrNoMemory;
	TRAPD(r, RunL());
	if (r != KErrNone)
		RProcess::Rendezvous(r);
	delete cleanup;
	return r;
	}
