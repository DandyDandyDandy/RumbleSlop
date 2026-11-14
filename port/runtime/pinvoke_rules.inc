// {method-name pattern ('*' = any run), kind, value, reason}; first match wins. Included by pinvoke_policy.c.

// nnUser vibration player (nnUserPlugin.Runtime.dll): implemented by the port (imports_hid.c, rt_vib_pinvoke),
// which rt_pinvoke_policy asks first; this rule only catches a native of it that the port does not know.
{"nnUser.Util+Vibration::*", P_RET, 0, "vibration file player: unknown native"},

// maria-upload telemetry (com.sega.techmng.maria-upload.dll): no-ops reporting success.
{"MariaUpload.*", P_RET, 0, "telemetry disabled"},

// NPLN (nn.npln.dll): the native library is not present. Library set-up and the hooks it would call back are
// accepted; destructors of native objects have nothing to destroy. Every other native (object creation, RPCs,
// accessors) still raises EntryPointNotFoundException: the game only reaches those after it has gone online, which
// the platform layer refuses first (no network, no network service account).
{"nn.npln.unityrt.RuntimeInitializer::SetupNativeLibrary", P_RET, 0, "NPLN offline: set-up accepted"},
{"nn.npln.unityrt.Methods::SetCanInvokeCsDelegateInFiber", P_RET, 0, "NPLN offline: set-up accepted"},
{"nn.npln.unityrt.GCHandleProxy::SetNativeFreeGCHandleFuncImpl", P_RET, 0, "NPLN offline: set-up accepted"},
{"nn.npln.unityrt.NativeMemory::Deallocate", P_RET, 0, "NPLN offline: nothing was allocated"},
{"nn.*::DtorImpl*", P_RET, 0, "NPLN offline: no native object"},
