#pragma once

// Four deliberately-misbehaving worker behaviors, each proving the host's
// SharedSectionValidator rejects one class of fault named in
// .docs/design/10-delivery-plan.md's Gate 2 exit criteria: mutating shared-
// section bytes after the host's first read, replaying a stale generation,
// and lying about a chunk's declared layout/offset (two variants). The
// fourth named fault, overrunning Job Object limits, is already proven by
// tests/import-isolation/SandboxLaunchTests.cpp's --overallocate/--hang
// tests against the real sandboxed launch mechanism -- not duplicated here.
//
// Each mode reads one StartGenerationRequest from the inherited stdin pipe
// and maps the inherited shared section, exactly like the honest worker's
// GenerationWorker::RunGeneration -- but this file deliberately does NOT
// call into GenerationWorker.cpp, so IsHostileTestBinary's relaxed warnings
// stay confined to genuinely test-only code and the real worker project is
// untouched.

namespace hostile_worker {

// Writes a byte-identical honest fixture (reusing
// import_worker::GenerateSyntheticScene), sends ChunksReadyNotice, then --
// on the same thread, after a fixed delay comfortably longer than any
// plausible host validation pass -- corrupts a few payload bytes in place.
// Proves the host's already-returned, privately-copied validation result is
// unaffected by mutation that happens after ChunksReady.
int RunMutateAfterReady();

// Writes an otherwise-honest section but stamps SectionHeader.generationId
// with a value other than what the request asked for (checksums correctly
// computed over what was actually written). Proves the host's generation
// staleness check rejects it even though every checksum is internally
// consistent.
int RunReplayGeneration();

// Hand-builds a single-chunk section whose descriptor claims a payload
// range extending past sectionLength, while every checksum is correct for
// what was actually written. Proves the bounds check rejects it
// independently of the checksum.
int RunLieOffset();

// Hand-builds a single-chunk section whose descriptor names a
// vertexLayoutId outside the closed VertexLayoutId enumeration, everything
// else honest. Proves the closed-enum lookup rejects it rather than
// guessing a stride.
int RunLieLayout();

// ---------------------------------------------------------------------------
// Progressive delivery (ChunkBatchReady/ChunkBatchConsumed).
//
// Every mode below is an attack that simply does not exist until a
// generation may write the output window more than once, which is why the
// four above could not already cover them. They are driven through
// import_broker::RunImportSession rather than a hand-rolled control channel,
// because the rules they attack -- batch ordering, the batch and chunk caps,
// and the ack handshake -- live in that function's own reply loop.
//
// These modes answer a StartXxxImportFromFile request (whatever
// RunImportSession sends) rather than StartGeneration, and ignore the source
// file handle entirely: what they fabricate is the *output*, which is the
// only thing the host trusts a worker for.
// ---------------------------------------------------------------------------

// Sends two well-formed batches and a terminal ChunksReady, all honest.
// The control case: proves the suite's own multi-batch machinery works, so a
// rejection in the modes below is the host's doing and not a broken fixture.
int RunHonestBatches();

// Sends batch 0, waits for its ack, then sends batch 0 again instead of
// batch 1. Proves a replayed index cannot re-present bytes the host already
// accepted and moved past.
int RunReplayBatchIndex();

// Sends batch 0, then batch 2 -- skipping 1. Proves the host tracks its own
// expected index rather than trusting the worker's claim.
int RunSkipBatchIndex();

// Sends a batch whose chunk reuses a chunkId the host already accepted in an
// earlier batch. Proves id uniqueness spans the generation, not just one
// section -- without which a later batch could take over what every
// already-accepted reference to that id resolves to.
int RunReuseChunkIdAcrossBatches();

// Keeps sending well-formed batches forever, never terminating. Proves the
// per-generation batch cap stops it rather than the host servicing batches
// until the worker chooses to stop.
int RunUnboundedBatches();

// Sends a batch and then, without waiting for its ack, immediately rewrites
// the section underneath the host and sends the next one. Proves the host's
// copy-then-validate snapshot of an accepted batch is unaffected -- the
// cross-batch form of the mutate-after-ready attack above, on a window that
// is now deliberately reused.
int RunWriteBeforeAck();

// Sends a terminal ChunksReady and then one more ChunkBatchReady after it.
// Proves the terminal reply really does end the generation.
int RunBatchAfterTerminal();

} // namespace hostile_worker
