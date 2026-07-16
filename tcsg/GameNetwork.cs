using Sandbox.Network;
using StbImageSharp;
using System;
using System.Threading.Tasks;

public sealed class GameNetwork : Component, Component.INetworkListener
{
	#region Constants

	private const int ChunkSize = 32 * 1024;
	private const long MaxTextureBytes = 16L * 1024 * 1024;
	private const int MaxChunkCount = (int)(MaxTextureBytes / ChunkSize) + 1;

	#endregion

	#region Static

	public static GameNetwork Instance { get; private set; }

	private static readonly Dictionary<string, byte[]> _textureBytes = new();
	private static readonly Dictionary<string, Texture> _textures = new();
	private static readonly Dictionary<string, Material> _materials = new();
	private static readonly Dictionary<Material, string> _pathFromMaterial = new();
	private static readonly Dictionary<string, List<Action<Material>>> _pendingCallbacks = new();
	private static readonly Dictionary<string, byte[][]> _incomingChunks = new();

	#endregion

	#region Fields

	private Connection _originalHost;

	#endregion

	#region Properties

	[Property]
	public GameObject PlayerPrefab { get; set; }

	[Property] public bool StartLobbyOnLoad { get; set; } = true;

	[Property] public bool StartServer { get; set; } = true;

	#endregion

	#region Lifecycle

	protected override void OnAwake()
	{
		base.OnAwake();
		Instance = this;
	}

	protected override async Task OnLoad()
	{
		if ( Scene.IsEditor )
			return;

		if ( StartServer && !Networking.IsActive )
		{
			LoadingScreen.Title = "Creating Lobby";
			await Task.DelayRealtimeSeconds( 0.1f );
			Networking.CreateLobby( new() );
		}
	}

	// No OnUpdate host-migration check: the engine briefly flickers
	// "host changed" pairs during routine peer joins/leaves, and a one-frame
	// "_wasGuest && IsHost" reading fires on those flickers too, kicking
	// whoever happened to be reported as the new host that frame — including
	// the original host themselves. OnDisconnected already handles the
	// legitimate "original host actually left" case by disconnecting the
	// guests, which is the only outcome the OnUpdate check was meant to
	// produce.

	protected override void OnDestroy()
	{
		base.OnDestroy();
		if ( Instance != this ) return;
		Instance = null;
		_textureBytes.Clear();
		_textures.Clear();
		_materials.Clear();
		_pathFromMaterial.Clear();
		_pendingCallbacks.Clear();
		_incomingChunks.Clear();
	}

	void INetworkListener.OnActive( Connection channel )
	{
		Log.Info( $"Player '{channel.DisplayName}' has joined the game" );

		if ( _originalHost is null && Networking.IsActive )
			_originalHost = Connection.Host;

		if ( !Networking.IsHost ) return;

		// Spawn a pawn for this connection — host included.
		if ( PlayerPrefab.IsValid() )
		{
			var player = PlayerPrefab.Clone( global::Transform.Zero, name: $"Player - {channel.DisplayName}" );
			player.NetworkSpawn( channel );
		}

		// Catch remote joiners up on world state; the host already has it.
		if ( channel != Connection.Local )
			SendInitialStateTo( channel );
	}

	void INetworkListener.OnDisconnected( Connection channel )
	{
		if ( _originalHost is not null && channel == _originalHost && !Networking.IsHost )
		{
			Networking.Disconnect();
			return;
		}

		if ( !Networking.IsHost ) return;

		InheritOrphanedBrushes( channel );
	}

	// Inherit any brushes whose owner just left, and proactively reclaim any
	// brush whose owner has gone null (this catches host-migration drift
	// where a previous host's reassignment never replicated to us). Brushes
	// that are already locally owned are skipped to avoid emitting redundant
	// AssignOwnership RPCs — those broadcast to every peer, and any peer that
	// doesn't happen to have the GameObject logs an "OnObjectMessage: Unknown
	// GameObject ... for RPC Msg_AssignOwnership" line. There's no way to
	// target the RPC at only peers that have the object, so the next best
	// thing is to avoid emitting it when there's nothing to change.
	private void InheritOrphanedBrushes( Connection leavingChannel )
	{
		var localConn = Connection.Local;
		var inherited = 0;
		var orphans = 0;
		foreach ( var brush in ModelBrush.Brushes )
		{
			if ( !brush.IsValid() ) continue;
			var go = brush.GameObject;
			if ( go is null ) continue;
			var net = go.Network;
			if ( net is null || !net.Active ) continue;

			var owner = net.Owner;
			if ( owner == localConn ) continue;

			var leftWithThisChannel = leavingChannel is not null && owner == leavingChannel;
			var orphaned = owner is null;
			if ( !leftWithThisChannel && !orphaned ) continue;

			try
			{
				net.AssignOwnership( localConn );
				if ( leftWithThisChannel ) inherited++;
				else orphans++;
			}
			catch ( Exception e )
			{
				Log.Warning( $"togethercsg: failed to inherit brush '{go.Name}' ({go.Id}): {e.Message}" );
			}
		}
		if ( inherited > 0 || orphans > 0 )
		{
			Log.Info( $"togethercsg: brush inheritance after '{leavingChannel?.DisplayName ?? "?"}' left — {inherited} from leaver, {orphans} orphan(s) reclaimed" );
		}
	}

	#endregion

	#region Texture Registry

	public static bool TryGetTexture( string path, out Texture texture )
	{
		return _textures.TryGetValue( path, out texture );
	}

	public static bool TryGetMaterial( string path, out Material material )
	{
		return _materials.TryGetValue( path, out material );
	}

	public static string GetPathForMaterial( Material material )
	{
		if ( material is null ) return null;
		return _pathFromMaterial.TryGetValue( material, out var p ) ? p : null;
	}

	public static void EnsureMaterial( string path, Action<Material> callback )
	{
		if ( string.IsNullOrEmpty( path ) || callback is null ) return;
		if ( _materials.TryGetValue( path, out var material ) )
		{
			callback( material );
			return;
		}
		if ( !_pendingCallbacks.TryGetValue( path, out var list ) )
		{
			_pendingCallbacks[path] = list = new List<Action<Material>>();
		}
		list.Add( callback );
	}

	// When a peer's RpcReceiveTextureChunk fires before their local Player has
	// finished OnStart, Player.Instance is null and BuildAndCache early-returns
	// before creating the Material (it still records the bytes + texture). The
	// queued EnsureMaterial callbacks from the brush snapshot then never fire,
	// so faces stay unmaterialised. Call this once Player.Instance is available
	// to backfill any missing materials.
	public static void RebuildPendingMaterials()
	{
		var template = Player.Instance?.TemplateMaterial;
		if ( template is null ) return;
		List<(string path, byte[] bytes)> pending = null;
		foreach ( var (path, bytes) in _textureBytes )
		{
			if ( _materials.ContainsKey( path ) ) continue;
			pending ??= new List<(string, byte[])>();
			pending.Add( (path, bytes) );
		}
		if ( pending is null ) return;
		foreach ( var (path, bytes) in pending )
		{
			BuildAndCache( path, bytes, template );
		}
	}

	public static void RegisterLocalTexture( string path, byte[] bytes, Material templateMaterial )
	{
		if ( string.IsNullOrEmpty( path ) || bytes is null || bytes.Length == 0 ) return;
		if ( bytes.Length > MaxTextureBytes )
		{
			Log.Warning( $"togethercsg: texture '{path}' exceeds {MaxTextureBytes} bytes, skipping" );
			return;
		}
		if ( !_materials.ContainsKey( path ) )
		{
			BuildAndCache( path, bytes, templateMaterial );
		}
		Instance?.BroadcastTextureBytes( path, bytes );
	}

	[Rpc.Broadcast]
	public void RpcRequestTexture( string path )
	{
		if ( !Networking.IsHost ) return;
		if ( string.IsNullOrEmpty( path ) ) return;
		if ( !_textureBytes.TryGetValue( path, out var bytes ) ) return;
		var caller = Rpc.Caller;
		if ( caller is null ) return;
		SendTextureBytesTo( caller, path, bytes );
	}

	[Rpc.Broadcast]
	public void RpcReceiveTextureChunk( string path, int chunkIndex, int totalChunks, byte[] data )
	{
		if ( string.IsNullOrEmpty( path ) || data is null ) return;
		if ( _textureBytes.ContainsKey( path ) ) return;
		if ( totalChunks <= 0 || totalChunks > MaxChunkCount ) return;
		if ( data.Length > ChunkSize ) return;

		if ( !_incomingChunks.TryGetValue( path, out var chunks ) || chunks.Length != totalChunks )
		{
			chunks = new byte[totalChunks][];
			_incomingChunks[path] = chunks;
		}
		if ( chunkIndex < 0 || chunkIndex >= totalChunks ) return;
		chunks[chunkIndex] = data;

		var totalLen = 0;
		for ( var i = 0; i < chunks.Length; i++ )
		{
			if ( chunks[i] is null ) return;
			totalLen += chunks[i].Length;
		}

		var bytes = new byte[totalLen];
		var offset = 0;
		foreach ( var c in chunks )
		{
			Buffer.BlockCopy( c, 0, bytes, offset, c.Length );
			offset += c.Length;
		}
		_incomingChunks.Remove( path );

		var template = Player.Instance?.TemplateMaterial;
		BuildAndCache( path, bytes, template );
	}

	private void SendInitialStateTo( Connection channel )
	{
		foreach ( var (path, bytes) in _textureBytes )
		{
			SendTextureBytesTo( channel, path, bytes );
		}
		foreach ( var brush in ModelBrush.Brushes )
		{
			if ( !brush.IsValid() ) continue;
			if ( !brush.GameObject.Network.Active ) continue;
			byte[] data;
			try
			{
				data = brush.SerializeMeshState();
			}
			catch ( Exception e )
			{
				Log.Warning( $"togethercsg: snapshot serialize failed: {e.Message}" );
				continue;
			}
			using ( Rpc.FilterInclude( channel ) )
			{
				brush.RpcApplyMeshSnapshot( data );
			}
		}
	}

	private void SendTextureBytesTo( Connection channel, string path, byte[] bytes )
	{
		SendChunks( channel, path, bytes );
	}

	private void BroadcastTextureBytes( string path, byte[] bytes )
	{
		SendChunks( null, path, bytes );
	}

	// Chunked texture send. `target == null` broadcasts to everyone; otherwise
	// each chunk goes inside a `Rpc.FilterInclude(target)` scope.
	private void SendChunks( Connection target, string path, byte[] bytes )
	{
		var totalChunks = Math.Max( 1, (bytes.Length + ChunkSize - 1) / ChunkSize );
		for ( var i = 0; i < totalChunks; i++ )
		{
			var off = i * ChunkSize;
			var len = Math.Min( ChunkSize, bytes.Length - off );
			var chunk = new byte[len];
			Buffer.BlockCopy( bytes, off, chunk, 0, len );
			if ( target is null )
			{
				RpcReceiveTextureChunk( path, i, totalChunks, chunk );
			}
			else
			{
				using ( Rpc.FilterInclude( target ) )
				{
					RpcReceiveTextureChunk( path, i, totalChunks, chunk );
				}
			}
		}
	}

	private static void BuildAndCache( string path, byte[] bytes, Material templateMaterial )
	{
		_textureBytes[path] = bytes;

		try
		{
			FileSystem.Data.WriteAllBytes( path, bytes );
		}
		catch ( Exception e )
		{
			Log.Warning( $"togethercsg: failed to cache texture '{path}' to disk: {e.Message}" );
		}

		ImageResult imageResult;
		try
		{
			imageResult = ImageResult.FromMemory( bytes, ColorComponents.RedGreenBlueAlpha );
		}
		catch
		{
			return;
		}

		var bitmap = new Bitmap( imageResult.Width, imageResult.Height );
		var colors = new Color[imageResult.Width * imageResult.Height];
		for ( var i = 0; i < colors.Length; i++ )
		{
			var r = imageResult.Data[i * 4 + 0];
			var g = imageResult.Data[i * 4 + 1];
			var b = imageResult.Data[i * 4 + 2];
			var a = imageResult.Data[i * 4 + 3];
			colors[i] = Color.FromBytes( r, g, b, a );
		}
		bitmap.SetPixels( colors );
		var texture = bitmap.ToTexture();
		_textures[path] = texture;

		if ( templateMaterial is null ) return;

		var material = templateMaterial.CreateCopy( $"togethercsg_face_{path}" );
		material.Set( "g_tTexture", texture );
		_materials[path] = material;
		_pathFromMaterial[material] = path;
		FlushPending( path, material );
	}

	private static void FlushPending( string path, Material material )
	{
		if ( !_pendingCallbacks.TryGetValue( path, out var list ) ) return;
		_pendingCallbacks.Remove( path );
		foreach ( var cb in list )
		{
			cb?.Invoke( material );
		}
	}

	#endregion
}
