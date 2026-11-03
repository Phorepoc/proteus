// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0
// proteusimg.h -- OURS: the image broker's protocol (proteusimg.cpp) and its client (proteus.cpp ProteusImgOpen).
#ifndef PROTEUSIMG_H
#define PROTEUSIMG_H

#include <e32std.h>
#include <f32file.h>

_LIT(KProteusImgServer, "!ProteusImages");
_LIT(KProteusImgExe, "proteusimg.exe");
enum { EProteusImgOpen = 1 };		// (const TDesC& path, TPckgBuf<TInt>& sub-session) -> file server session handle

// open an ARM image under \sys\bin\ through the broker (starting it when it is not running); aFile is read-only
inline TInt ProteusImgOpen(const TDesC& aPath, RFile& aFile)
	{
	class RImg : public RSessionBase
		{
	public:
		TInt Connect()
			{
			TInt r = CreateSession(KProteusImgServer, TVersion(1, 0, 0), 1);
			if (r != KErrNotFound && r != KErrServerTerminated)
				return r;
			RProcess p;
			r = p.Create(KProteusImgExe, KNullDesC);
			if (r != KErrNone)
				return r;
			TRequestStatus st;
			p.Rendezvous(st);
			p.Resume();
			User::WaitForRequest(st);
			r = p.ExitType() == EExitPanic ? KErrGeneral : st.Int();
			p.Close();
			return r == KErrNone || r == KErrAlreadyExists ? CreateSession(KProteusImgServer, TVersion(1, 0, 0), 1) : r;
			}
		TInt Open(const TDesC& aPath, RFile& aFile)
			{
			TPckgBuf<TInt> sub;
			const TInt fsh = SendReceive(EProteusImgOpen, TIpcArgs(&aPath, &sub));
			return fsh < 0 ? fsh : aFile.AdoptFromServer(fsh, sub());
			}
		};
	RImg s;
	TInt r = s.Connect();
	if (r == KErrNone)
		r = s.Open(aPath, aFile);
	s.Close();
	return r;
	}

#endif
