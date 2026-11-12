//
//  Transcode.h
//  iSH-AOK
//
//  The hardware video encoder behind /AOK/native/transcode and the Workspace
//  Video Converter applet. See kernel/native_transcode.h for the job.
//

#include "kernel/native_transcode.h"

// Runs the job on the calling thread, which it blocks until the job ends (for
// a long video, minutes); the job's callbacks are called on that thread too.
// So: never the main thread.
int ISHTranscodeRun(struct transcode_job *job);
