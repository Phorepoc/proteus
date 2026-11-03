// proteusfs.cpp -- OURS (Proteus P2, docs/phorepoc/proteus-p2.md): ARM code -> ARM efsrv -> (IPC bridge) -> the NATIVE x86 file server.
// Connects, reads the drive list, writes a file into its private directory and reads it back.
#include <e32std.h>
#include <e32debug.h>
#include <f32file.h>

GLDEF_C TInt E32Main()
	{
	RFs fs;
	TInt r = fs.Connect();
	RDebug::Printf("PH: proteus ARM fs: RFs::Connect -> %d", r);
	if (r != KErrNone)
		return r;
	TDriveList drives;
	r = fs.DriveList(drives);
	TInt n = 0;
	for (TInt i = 0; i < KMaxDrives; ++i) if (drives[i]) ++n;
	RDebug::Printf("PH: proteus ARM fs: DriveList -> %d, %d drives, C: %d, Z: %d", r, n, drives[EDriveC] != 0, drives[EDriveZ] != 0);
	// the private directory: created on C:, then the session's own path (a new session starts there) -- under Proteus that is the
	// guest's \private\<SecureId>\, not the handler's
	r = fs.CreatePrivatePath(EDriveC);
	TFileName priv;
	const TInt rp = fs.PrivatePath(priv);
	TBuf8<64> priv8;
	priv8.Copy(priv);
	RDebug::Printf("PH: proteus ARM fs: CreatePrivatePath -> %d, PrivatePath -> %d %S", r, rp, &priv8);
	if (r != KErrNone || rp != KErrNone || (r = fs.SetSessionToPrivate(EDriveC)) != KErrNone)
		return r != KErrNone ? r : rp;
	_LIT(KFile, "proteusfs.txt");
	_LIT8(KText, "written by ARM code through the native file server");
	RFile f;
	r = f.Replace(fs, KFile, EFileWrite);
	if (r == KErrNone)
		{
		r = f.Write(KText);
		f.Close();
		}
	RDebug::Printf("PH: proteus ARM fs: write -> %d", r);
	TBuf8<80> back;
	if (r == KErrNone && (r = f.Open(fs, KFile, EFileRead)) == KErrNone)
		{
		r = f.Read(back);
		f.Close();
		}
	const TBool ok = r == KErrNone && back == KText && drives[EDriveZ];
	RDebug::Printf("PH: proteus ARM fs: read back %d bytes -> %d, %s", back.Length(), r, ok ? "ok" : "WRONG");
	fs.Delete(KFile);
	fs.Close();
	return ok ? KErrNone : KErrGeneral;
	}
