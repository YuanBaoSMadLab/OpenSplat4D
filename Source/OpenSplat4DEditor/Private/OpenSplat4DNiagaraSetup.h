#pragma once

namespace OpenSplat4DNiagaraSetup
{
	bool EnsureAssetsExist();
	bool EnsureAssetsExistFromTeacherTemplate();
	bool EnsureAssetsExistFromBundledTemplate();

	/**
	 * Programmatically build the complete NS_OpenSplat4D Niagara system
	 * with DI function call nodes in the GPU Compute Script graph.
	 * This replaces the old Scratch-Pad-based approach that is not
	 * supported in UE 5.8+.
	 */
	bool BuildNiagaraSystem();
}
