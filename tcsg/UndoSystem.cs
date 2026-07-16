using Sandbox;
using System;
using System.Collections.Generic;

// Generic snapshot-based undo/redo for brush edits.
//
// Each edit transaction snapshots the state of a known set of brushes "before"
// the change and "after" the change. State = { Exists, WorldTransform components,
// SerializedMesh bytes }. To reverse the change we apply the "before" state to
// each tracked brush; to replay it we apply the "after" state.
//
// Brushes are addressed via a `BrushSlot` indirection. When an Undo has to
// recreate a previously-destroyed brush, the slot follows the new instance, so
// older history entries that still reference the same slot keep working.
//
// All changes are made via existing edit paths (transform writes, mesh
// snapshot RPCs, prefab spawn) so peers see them through the normal sync.
// History itself is local — each client has their own undo stack of their
// own edits.
public sealed class UndoSystem
{
	public sealed class BrushSlot
	{
		public ModelBrush Brush;
	}

	public struct BrushState
	{
		public bool Exists;
		public Vector3 Position;
		public Rotation Rotation;
		public Vector3 Scale;
		public byte[] MeshState;
	}

	// Tracks a cloud-spawned GameObject (model prop or entity sprite placeholder).
	// "Exists" is reconciled via GameObject.Enabled — undoing a delete re-enables
	// the existing instance rather than respawning async cloud assets.
	public sealed class CloudSlot
	{
		public CloudInstance Instance;
	}

	public struct CloudState
	{
		public bool Active;
		public Vector3 Position;
		public Rotation Rotation;
		public Vector3 Scale;
	}

	private sealed class Entry
	{
		public Dictionary<BrushSlot, BrushState> Before;
		public Dictionary<BrushSlot, BrushState> After;
		public Dictionary<CloudSlot, CloudState> CloudBefore;
		public Dictionary<CloudSlot, CloudState> CloudAfter;
	}

	private const int MaxHistory = 100;

	private readonly List<Entry> _history = new();
	// _cursor is the number of entries from _history that are currently
	// "applied". Undo decrements; Redo increments.
	private int _cursor = 0;

	private readonly Dictionary<ModelBrush, BrushSlot> _slotByBrush = new();
	private readonly Dictionary<CloudInstance, CloudSlot> _slotByCloud = new();
	private Dictionary<BrushSlot, BrushState> _pending;
	private Dictionary<CloudSlot, CloudState> _pendingCloud;
	private bool _suspended;

	public bool CanUndo => !_suspended && _cursor > 0;
	public bool CanRedo => !_suspended && _cursor < _history.Count;

	// === Snapshot helpers ===

	private BrushState Snapshot( BrushSlot slot )
	{
		var brush = slot.Brush;
		if ( brush is null || !brush.IsValid() )
		{
			return new BrushState { Exists = false };
		}
		byte[] mesh = null;
		try
		{
			mesh = brush.SerializeMeshState();
		}
		catch ( Exception e )
		{
			Log.Warning( $"togethercsg: undo snapshot serialise failed: {e.Message}" );
		}
		return new BrushState
		{
			Exists = true,
			Position = brush.WorldPosition,
			Rotation = brush.WorldRotation,
			Scale = brush.WorldScale,
			MeshState = mesh,
		};
	}

	public BrushSlot GetOrCreateSlot( ModelBrush brush )
	{
		if ( brush is null ) return null;
		if ( !_slotByBrush.TryGetValue( brush, out var slot ) )
		{
			slot = new BrushSlot { Brush = brush };
			_slotByBrush[brush] = slot;
		}
		return slot;
	}

	public CloudSlot GetOrCreateCloudSlot( CloudInstance instance )
	{
		if ( instance is null ) return null;
		if ( !_slotByCloud.TryGetValue( instance, out var slot ) )
		{
			slot = new CloudSlot { Instance = instance };
			_slotByCloud[instance] = slot;
		}
		return slot;
	}

	private CloudState SnapshotCloud( CloudSlot slot )
	{
		var ci = slot.Instance;
		if ( ci is null || !ci.IsValid() || ci.GameObject is null )
		{
			return new CloudState { Active = false };
		}
		return new CloudState
		{
			Active = ci.GameObject.Enabled,
			Position = ci.WorldPosition,
			Rotation = ci.WorldRotation,
			Scale = ci.WorldScale,
		};
	}

	// === Transaction API ===

	// Open a new edit transaction. Existing brushes whose state may change
	// during the transaction must be passed in (or added via TrackExisting)
	// so their "before" state is captured for undo.
	public void BeginEdit( IEnumerable<ModelBrush> existing = null )
	{
		if ( _suspended ) return;
		// If an edit was left open (caller forgot to End), drop it silently.
		_pending = new Dictionary<BrushSlot, BrushState>();
		_pendingCloud = new Dictionary<CloudSlot, CloudState>();
		if ( existing is null ) return;
		foreach ( var brush in existing )
		{
			TrackExisting( brush );
		}
	}

	public void BeginEditCloud( IEnumerable<CloudInstance> existing = null )
	{
		if ( _suspended ) return;
		_pending = new Dictionary<BrushSlot, BrushState>();
		_pendingCloud = new Dictionary<CloudSlot, CloudState>();
		if ( existing is null ) return;
		foreach ( var ci in existing )
		{
			TrackExistingCloud( ci );
		}
	}

	public void TrackExistingCloud( CloudInstance instance )
	{
		if ( _suspended || _pendingCloud is null || instance is null ) return;
		var slot = GetOrCreateCloudSlot( instance );
		if ( !_pendingCloud.ContainsKey( slot ) )
		{
			_pendingCloud[slot] = SnapshotCloud( slot );
		}
	}

	public CloudSlot TrackNewCloud( CloudInstance instance )
	{
		if ( instance is null ) return null;
		var slot = GetOrCreateCloudSlot( instance );
		if ( _suspended || _pendingCloud is null ) return slot;
		if ( !_pendingCloud.ContainsKey( slot ) )
		{
			// Before-state: not active (so undo will deactivate the new instance).
			_pendingCloud[slot] = new CloudState { Active = false };
		}
		return slot;
	}

	public void TrackExisting( ModelBrush brush )
	{
		if ( _suspended || _pending is null || brush is null ) return;
		var slot = GetOrCreateSlot( brush );
		if ( !_pending.ContainsKey( slot ) )
		{
			_pending[slot] = Snapshot( slot );
		}
	}

	// Call right after GameObject.Destroy() on a tracked brush. The sbox
	// destroy is deferred — Snapshot() ran during EndEdit() could still
	// see brush.IsValid() == true on this frame, which made the
	// "After" snapshot match the "Before" snapshot, HasDiff() returned
	// false, and the delete left no entry on the undo history. Nulling
	// the slot here forces the next Snapshot() down the Exists=false
	// branch unconditionally so the diff is recorded.
	public void MarkBrushDestroyed( ModelBrush brush )
	{
		if ( _suspended || brush is null ) return;
		if ( !_slotByBrush.TryGetValue( brush, out var slot ) ) return;
		slot.Brush = null;
		_slotByBrush.Remove( brush );
	}

	// Call right after a new brush is spawned during the active transaction.
	public BrushSlot TrackNewBrush( ModelBrush brush )
	{
		if ( brush is null ) return null;
		var slot = GetOrCreateSlot( brush );
		if ( _suspended || _pending is null ) return slot;
		if ( !_pending.ContainsKey( slot ) )
		{
			_pending[slot] = new BrushState { Exists = false };
		}
		return slot;
	}

	// Close the transaction and push to history (if any state actually
	// changed). Truncates the redo stack past the cursor.
	public void EndEdit()
	{
		if ( _suspended ) return;
		var before = _pending;
		var cloudBefore = _pendingCloud;
		_pending = null;
		_pendingCloud = null;
		var hasBrush = before is not null && before.Count > 0;
		var hasCloud = cloudBefore is not null && cloudBefore.Count > 0;
		if ( !hasBrush && !hasCloud ) return;

		Dictionary<BrushSlot, BrushState> after = null;
		if ( hasBrush )
		{
			after = new Dictionary<BrushSlot, BrushState>( before.Count );
			foreach ( var slot in before.Keys )
			{
				after[slot] = Snapshot( slot );
			}
		}

		Dictionary<CloudSlot, CloudState> cloudAfter = null;
		if ( hasCloud )
		{
			cloudAfter = new Dictionary<CloudSlot, CloudState>( cloudBefore.Count );
			foreach ( var slot in cloudBefore.Keys )
			{
				cloudAfter[slot] = SnapshotCloud( slot );
			}
		}

		var brushDiff = hasBrush && HasDiff( before, after );
		var cloudDiff = hasCloud && HasCloudDiff( cloudBefore, cloudAfter );
		if ( !brushDiff && !cloudDiff ) return;

		if ( _cursor < _history.Count )
		{
			_history.RemoveRange( _cursor, _history.Count - _cursor );
		}
		_history.Add( new Entry
		{
			Before = brushDiff ? before : null,
			After = brushDiff ? after : null,
			CloudBefore = cloudDiff ? cloudBefore : null,
			CloudAfter = cloudDiff ? cloudAfter : null,
		} );
		if ( _history.Count > MaxHistory )
		{
			_history.RemoveAt( 0 );
		}
		else
		{
			_cursor++;
		}
	}

	public void CancelEdit()
	{
		_pending = null;
		_pendingCloud = null;
	}

	// Drop the entire undo / redo history. Used when the editor wipes the
	// scene (Reset): every recorded entry references brushes that no longer
	// exist, so leaving them in place would let Undo try to resurrect a
	// half-destroyed world.
	public void Clear()
	{
		_history.Clear();
		_cursor = 0;
		_slotByBrush.Clear();
		_slotByCloud.Clear();
		_pending = null;
		_pendingCloud = null;
		_suspended = false;
	}

	private static bool HasDiff( Dictionary<BrushSlot, BrushState> a, Dictionary<BrushSlot, BrushState> b )
	{
		foreach ( var (slot, sa) in a )
		{
			if ( !b.TryGetValue( slot, out var sb ) ) return true;
			if ( sa.Exists != sb.Exists ) return true;
			if ( !sa.Exists ) continue;
			if ( sa.Position != sb.Position ) return true;
			if ( sa.Rotation != sb.Rotation ) return true;
			if ( sa.Scale != sb.Scale ) return true;
			if ( !ByteArraysEqual( sa.MeshState, sb.MeshState ) ) return true;
		}
		return false;
	}

	private static bool HasCloudDiff( Dictionary<CloudSlot, CloudState> a, Dictionary<CloudSlot, CloudState> b )
	{
		foreach ( var (slot, sa) in a )
		{
			if ( !b.TryGetValue( slot, out var sb ) ) return true;
			if ( sa.Active != sb.Active ) return true;
			if ( !sa.Active ) continue;
			if ( sa.Position != sb.Position ) return true;
			if ( sa.Rotation != sb.Rotation ) return true;
			if ( sa.Scale != sb.Scale ) return true;
		}
		return false;
	}

	private static bool ByteArraysEqual( byte[] a, byte[] b )
	{
		if ( ReferenceEquals( a, b ) ) return true;
		if ( a is null || b is null ) return false;
		if ( a.Length != b.Length ) return false;
		for ( var i = 0; i < a.Length; i++ )
		{
			if ( a[i] != b[i] ) return false;
		}
		return true;
	}

	// === Apply (Undo/Redo) ===

	public void Undo()
	{
		if ( !CanUndo ) return;
		_cursor--;
		Apply( _history[_cursor].Before, _history[_cursor].CloudBefore );
	}

	public void Redo()
	{
		if ( !CanRedo ) return;
		Apply( _history[_cursor].After, _history[_cursor].CloudAfter );
		_cursor++;
	}

	private void Apply( Dictionary<BrushSlot, BrushState> target, Dictionary<CloudSlot, CloudState> cloudTarget )
	{
		// Suspend so any per-action hooks downstream don't try to open a
		// nested edit transaction while we're restoring state.
		_suspended = true;
		try
		{
			if ( target is not null )
			{
				// Pass 1: destroy brushes that should not exist.
				foreach ( var (slot, state) in target )
				{
					if ( state.Exists ) continue;
					var brush = slot.Brush;
					if ( brush is null || !brush.IsValid() ) continue;
					if ( _slotByBrush.TryGetValue( brush, out var mapped ) && mapped == slot )
					{
						_slotByBrush.Remove( brush );
					}
					brush.GameObject?.Destroy();
					slot.Brush = null;
				}

				// Pass 2: recreate brushes that should exist but currently don't.
				foreach ( var (slot, state) in target )
				{
					if ( !state.Exists ) continue;
					var brush = slot.Brush;
					if ( brush is not null && brush.IsValid() ) continue;

					var newBrush = RecreateBrush( state );
					if ( newBrush is null ) continue;
					slot.Brush = newBrush;
					_slotByBrush[newBrush] = slot;
				}

				// Pass 3: update transforms / meshes on still-existing brushes.
				foreach ( var (slot, state) in target )
				{
					if ( !state.Exists ) continue;
					var brush = slot.Brush;
					if ( brush is null || !brush.IsValid() ) continue;

					brush.WorldPosition = state.Position;
					brush.WorldRotation = state.Rotation;
					brush.WorldScale = state.Scale;
					if ( state.MeshState is null ) continue;
					ApplyMeshState( brush, state.MeshState );
				}
			}

			if ( cloudTarget is not null )
			{
				// Cloud objects use soft-destroy (Enabled = false) instead of full
				// destruction, so we can undo deletes without round-tripping the
				// cloud package loader. Newly-created instances flip from inactive
				// to active when re-doing; undo deactivates them.
				foreach ( var (slot, state) in cloudTarget )
				{
					var ci = slot.Instance;
					if ( ci is null || !ci.IsValid() || ci.GameObject is null ) continue;

					if ( state.Active )
					{
						ci.WorldPosition = state.Position;
						ci.WorldRotation = state.Rotation;
						ci.WorldScale = state.Scale;
					}
					if ( ci.GameObject.Enabled != state.Active )
					{
						if ( Networking.IsActive )
						{
							ci.RpcSetActive( state.Active );
						}
						else
						{
							ci.GameObject.Enabled = state.Active;
						}
					}
				}
			}
		}
		finally
		{
			_suspended = false;
		}

		Player.Instance?.OnUndoRedoApplied();
	}

	private static ModelBrush RecreateBrush( BrushState state )
	{
		var player = Player.Instance;
		if ( player is null ) return null;
		var prefab = player.GetSpawnPrefab();
		if ( prefab is null ) return null;

		var go = prefab.Clone();
		if ( go is null ) return null;
		go.WorldPosition = state.Position;
		go.WorldRotation = state.Rotation;
		go.WorldScale = state.Scale;

		if ( Networking.IsActive )
		{
			go.NetworkSpawn();
		}

		var newBrush = go.GetComponent<ModelBrush>();
		if ( newBrush is null ) return null;
		if ( state.MeshState is not null )
		{
			ApplyMeshState( newBrush, state.MeshState );
		}
		return newBrush;
	}

	private static void ApplyMeshState( ModelBrush brush, byte[] meshState )
	{
		brush.DeserializeMeshState( meshState );
		if ( !Networking.IsActive ) return;
		if ( !brush.CanLocalEdit() ) return;
		using ( Rpc.FilterExclude( Connection.Local ) )
		{
			brush.RpcApplyMeshSnapshot( meshState );
		}
	}
}
