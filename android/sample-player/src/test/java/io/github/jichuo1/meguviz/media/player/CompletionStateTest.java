package io.github.jichuo1.meguviz.media.player;

public final class CompletionStateTest {
    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }
    public static void main(String[] args) {
        CompletionState state = new CompletionState();
        require(!state.needsPoll() && !state.deviceLost(), "initial state");
        state.accepted(); state.accepted();
        state.completed(0, -8);
        require(state.needsPoll(), "non-job backpressure must not consume a completion");
        state.completed(1, -9);
        require(state.needsPoll() && !state.deviceLost(), "deadline is not device loss");
        state.completed(2, -13);
        require(!state.needsPoll() && !state.deviceLost(), "cancel clears last completed job");
        state.accepted();
        state.completed(3, CompletionState.DEVICE_LOST);
        require(state.deviceLost() && !state.needsPoll(), "device failure stops polling");
        state.newEpoch();
        require(state.deviceLost(), "a new epoch cannot recover a lost device");
        boolean rejected = false;
        try { state.accepted(); } catch (IllegalStateException expected) { rejected = true; }
        require(rejected, "lost renderer cannot accept more work");
        CompletionState normal = new CompletionState();normal.accepted();normal.newEpoch();normal.accepted();
        require(normal.needsPoll() && !normal.deviceLost(), "ordinary epoch changes stay usable");
        System.out.println("PASS completion accounting and sticky device loss");
    }
}
