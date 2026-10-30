#pragma once

// Released PSBFile trees are freed a few containers per frame. During exit
// nothing may wait for a later frame: the script engine goes away first.
void TVPSetPSBDeferredRelease(bool enabled);
void TVPFlushPSBDeferredRelease();
