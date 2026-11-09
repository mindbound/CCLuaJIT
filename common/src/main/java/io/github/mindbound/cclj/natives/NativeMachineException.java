package io.github.mindbound.cclj.natives;

/**
 * {@link NativeMachine#create} failed: out of memory, or the API description or the bios did not load. The message
 * is the reason as the native machine gives it, for example {@code bios.lua:1: unexpected symbol near '='}.
 */
public final class NativeMachineException extends Exception {
    public NativeMachineException(String message) {
        super(message);
    }
}
