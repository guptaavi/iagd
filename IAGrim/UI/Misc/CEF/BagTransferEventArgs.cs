namespace IAGrim.UI.Misc.CEF {
    public sealed class BagTransferEventArgs : EventArgs {
        public IReadOnlyList<long> PlayerItemIds { get; }
        public int BatchSize { get; }
        public int NumQueued { get; set; }
        public bool IsSuccessful { get; set; }

        public BagTransferEventArgs(IEnumerable<long> playerItemIds, int batchSize) {
            PlayerItemIds = playerItemIds.ToList();
            BatchSize = batchSize;
        }
    }
}
