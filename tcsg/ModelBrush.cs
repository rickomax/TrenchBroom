using HalfEdgeMesh;
using LibCSG;
using System;
using System.IO;
using System.Linq;
using System.Text.Json;
using static Sandbox.PolygonMesh;

public class SphereModelBrush : ModelBrush
{
	#region Constants

	public const int DefaultSlices = 8;
	public const int DefaultStacks = 8;
	// Bounds chosen to avoid degenerate meshes at the low end and
	// runaway vertex counts at the high end. 4 keeps the sphere a
	// recognisable shape; 128 is already a very dense subdivision.
	public const int MinSlices = 4;
	public const int MaxSlices = 128;
	public const int MinStacks = 4;
	public const int MaxStacks = 128;
	private const float Radius = 0.5f;

	#endregion

	#region Fields

	public int Slices = DefaultSlices;

	public int Stacks = DefaultStacks;

	#endregion

	#region Segment RPC

	[Rpc.Broadcast]
	public void RpcSetSegments( int slices, int stacks )
	{
		if ( !this.IsValid() ) return;
		Slices = Math.Clamp( slices, MinSlices, MaxSlices );
		Stacks = Math.Clamp( stacks, MinStacks, MaxStacks );
		RegenerateBuildMesh();
	}

	#endregion

	#region Build

	protected override void Build()
	{
		var ringVerts = new VertexHandle[Stacks - 1, Slices];
		var southPole = _polygonMesh.AddVertex( new Vector3( 0f, 0f, -Radius ) );
		for ( int i = 1; i < Stacks; i++ )
		{
			float phi = (float)i / Stacks * MathF.PI;
			float z = -MathF.Cos( phi ) * Radius;
			float ringR = MathF.Sin( phi ) * Radius;

			for ( int j = 0; j < Slices; j++ )
			{
				float theta = (float)j / Slices * TwoPi;
				float x = MathF.Cos( theta ) * ringR;
				float y = MathF.Sin( theta ) * ringR;
				ringVerts[i - 1, j] = _polygonMesh.AddVertex( new Vector3( x, y, z ) );
			}
		}
		var northPole = _polygonMesh.AddVertex( new Vector3( 0f, 0f, Radius ) );
		var triVerts = new VertexHandle[3];
		var quadVerts = new VertexHandle[4];
		var triUVs = new Vector2[3];
		var quadUVs = new Vector2[4];
		for ( int j = 0; j < Slices; j++ )
		{
			int j1 = (j + 1) % Slices;
			float u0 = (float)j / Slices;
			float u1 = (float)(j + 1) / Slices;
			float vRing = 1f - 1f / Stacks;

			triVerts[0] = southPole;
			triVerts[1] = ringVerts[0, j1];
			triVerts[2] = ringVerts[0, j];

			triUVs[0] = new Vector2( (u0 + u1) * 0.5f, 1f );
			triUVs[1] = new Vector2( u1, vRing );
			triUVs[2] = new Vector2( u0, vRing );

			var face = _polygonMesh.AddFace( triVerts );
			_polygonMesh.SetFaceTextureCoords( face, triUVs );
		}
		for ( int i = 0; i < Stacks - 2; i++ )
		{
			float vLow = 1f - (float)(i + 1) / Stacks;
			float vHigh = 1f - (float)(i + 2) / Stacks;

			for ( int j = 0; j < Slices; j++ )
			{
				int j1 = (j + 1) % Slices;
				float u0 = (float)j / Slices;
				float u1 = (float)(j + 1) / Slices;

				quadVerts[0] = ringVerts[i, j];
				quadVerts[1] = ringVerts[i, j1];
				quadVerts[2] = ringVerts[i + 1, j1];
				quadVerts[3] = ringVerts[i + 1, j];

				quadUVs[0] = new Vector2( u0, vLow );
				quadUVs[1] = new Vector2( u1, vLow );
				quadUVs[2] = new Vector2( u1, vHigh );
				quadUVs[3] = new Vector2( u0, vHigh );

				var face = _polygonMesh.AddFace( quadVerts );
				_polygonMesh.SetFaceTextureCoords( face, quadUVs );
			}
		}
		int topRing = Stacks - 2;
		for ( int j = 0; j < Slices; j++ )
		{
			int j1 = (j + 1) % Slices;
			float u0 = (float)j / Slices;
			float u1 = (float)(j + 1) / Slices;
			float vRing = 1f / Stacks;

			triVerts[0] = ringVerts[topRing, j];
			triVerts[1] = ringVerts[topRing, j1];
			triVerts[2] = northPole;

			triUVs[0] = new Vector2( u0, vRing );
			triUVs[1] = new Vector2( u1, vRing );
			triUVs[2] = new Vector2( (u0 + u1) * 0.5f, 0f );

			var face = _polygonMesh.AddFace( triVerts );
			_polygonMesh.SetFaceTextureCoords( face, triUVs );
		}
		_triangleCount = 2 * Slices * (Stacks - 1);
		base.Build();
	}

	#endregion
}

public class CylinderModelBrush : ModelBrush
{
	#region Constants

	public const int DefaultSlices = 8;
	// Same reasoning as SphereModelBrush: 4 keeps the cylinder
	// recognisable, 128 caps the geometry density.
	public const int MinSlices = 4;
	public const int MaxSlices = 128;
	private const float Radius = 0.5f;
	private const float HalfHeight = 0.5f;

	#endregion

	#region Fields

	public int Slices = DefaultSlices;

	#endregion

	#region Segment RPC

	[Rpc.Broadcast]
	public void RpcSetSlices( int slices )
	{
		if ( !this.IsValid() ) return;
		Slices = Math.Clamp( slices, MinSlices, MaxSlices );
		RegenerateBuildMesh();
	}

	#endregion

	#region Build

	protected override void Build()
	{
		// Single-N-gon caps (no central hub vertex). Matches what MAP round-trip
		// produces — a brush is the intersection of its half-spaces, so each
		// plane yields exactly one polygon. The old radial-fan caps introduced
		// a useless centre vertex that downstream ops (slice, boolean) had to
		// drag around for no shape benefit.
		var bottomRing = new VertexHandle[Slices];
		var topRing = new VertexHandle[Slices];
		for ( int j = 0; j < Slices; j++ )
		{
			float theta = (float)j / Slices * TwoPi;
			float x = MathF.Cos( theta ) * Radius;
			float y = MathF.Sin( theta ) * Radius;
			bottomRing[j] = _polygonMesh.AddVertex( new Vector3( x, y, -HalfHeight ) );
			topRing[j] = _polygonMesh.AddVertex( new Vector3( x, y, HalfHeight ) );
		}
		var quadVerts = new VertexHandle[4];
		var quadUVs = new Vector2[4];
		var capVerts = new VertexHandle[Slices];
		var capUVs = new Vector2[Slices];

		// Bottom cap: wind in decreasing θ so the outward normal is -Z.
		for ( int j = 0; j < Slices; j++ )
		{
			int srcJ = (Slices - j) % Slices;
			float theta = (float)srcJ / Slices * TwoPi;
			capVerts[j] = bottomRing[srcJ];
			capUVs[j] = new Vector2( 0.5f + MathF.Cos( theta ) * 0.5f, 0.5f - MathF.Sin( theta ) * 0.5f );
		}
		var bottomFace = _polygonMesh.AddFace( capVerts );
		_polygonMesh.SetFaceTextureCoords( bottomFace, capUVs );

		// Side wall quads.
		for ( int j = 0; j < Slices; j++ )
		{
			int j1 = (j + 1) % Slices;
			float u0 = (float)j / Slices;
			float u1 = (float)(j + 1) / Slices;

			quadVerts[0] = bottomRing[j];
			quadVerts[1] = bottomRing[j1];
			quadVerts[2] = topRing[j1];
			quadVerts[3] = topRing[j];

			quadUVs[0] = new Vector2( u0, 1f );
			quadUVs[1] = new Vector2( u1, 1f );
			quadUVs[2] = new Vector2( u1, 0f );
			quadUVs[3] = new Vector2( u0, 0f );

			var face = _polygonMesh.AddFace( quadVerts );
			_polygonMesh.SetFaceTextureCoords( face, quadUVs );
		}

		// Top cap: wind in increasing θ so the outward normal is +Z.
		for ( int j = 0; j < Slices; j++ )
		{
			float theta = (float)j / Slices * TwoPi;
			capVerts[j] = topRing[j];
			capUVs[j] = new Vector2( 0.5f + MathF.Cos( theta ) * 0.5f, 0.5f + MathF.Sin( theta ) * 0.5f );
		}
		var topFace = _polygonMesh.AddFace( capVerts );
		_polygonMesh.SetFaceTextureCoords( topFace, capUVs );

		// 2N tris for side quads, (N - 2) tris each for the two fan-triangulated
		// cap N-gons → 4N - 4. _triangleCount gets recomputed from the model
		// after base.Build() rebuilds it for ops that mutate later, but the
		// initial value here gates RpcSetFaceMaterial bounds checks.
		_triangleCount = 4 * Slices - 4;
		base.Build();
	}

	#endregion
}

public class BoxModelBrush : ModelBrush
{
	#region Constants

	private const int BoxTriangleCount = 12;

	#endregion

	#region Build

	protected override void Build()
	{
		var vertexHandles = new VertexHandle[8];
		vertexHandles[0] = _polygonMesh.AddVertex( new Vector3( -0.5f, -0.5f, -0.5f ) );
		vertexHandles[1] = _polygonMesh.AddVertex( new Vector3( 0.5f, -0.5f, -0.5f ) );
		vertexHandles[2] = _polygonMesh.AddVertex( new Vector3( 0.5f, 0.5f, -0.5f ) );
		vertexHandles[3] = _polygonMesh.AddVertex( new Vector3( -0.5f, 0.5f, -0.5f ) );
		vertexHandles[4] = _polygonMesh.AddVertex( new Vector3( -0.5f, -0.5f, 0.5f ) );
		vertexHandles[5] = _polygonMesh.AddVertex( new Vector3( 0.5f, -0.5f, 0.5f ) );
		vertexHandles[6] = _polygonMesh.AddVertex( new Vector3( 0.5f, 0.5f, 0.5f ) );
		vertexHandles[7] = _polygonMesh.AddVertex( new Vector3( -0.5f, 0.5f, 0.5f ) );
		var faceIndices =
		new int[][]{
			new[] { 1, 2, 6, 5 },
			new[] { 3, 0, 4, 7 },
			new[] { 2, 3, 7, 6 },
			new[] { 0, 1, 5, 4 },
			new[] { 4, 5, 6, 7 },
			new[] { 3, 2, 1, 0 },
		};
		var uvs = new[]
		{
			new Vector2( 0f, 1f ),
			new Vector2( 1f, 1f ),
			new Vector2( 1f, 0f ),
			new Vector2( 0f, 0f ),
		};
		var faceVerts = new VertexHandle[4];
		foreach ( var idx in faceIndices )
		{
			faceVerts[0] = vertexHandles[idx[0]];
			faceVerts[1] = vertexHandles[idx[1]];
			faceVerts[2] = vertexHandles[idx[2]];
			faceVerts[3] = vertexHandles[idx[3]];
			var face = _polygonMesh.AddFace( faceVerts );
			_polygonMesh.SetFaceTextureCoords( face, uvs );
		}
		_triangleCount = BoxTriangleCount;
		base.Build();
	}

	#endregion
}

public class ModelBrush : Component
{
	#region Constants

	public const float DefaultUVScale = 0.25f;

	protected const float VertexMatchEpsilon = 0.001f;
	protected static readonly float TwoPi = MathF.PI * 2f;

	private const float VertexGizmoSize = 5f;
	private const float BooleanCoplanarEpsilon = 0.0005f;
	private const float ConvexityEpsilon = 0.01f;
	private const float BooleanWeldEpsilon = 0.01f;

	#endregion

	#region Static

	public static HashSet<ModelBrush> Brushes { get; internal set; } = new HashSet<ModelBrush>();

	private static readonly Color _hoverColor = Color.Yellow;
	// Use the same yellow for selection: selected brushes get hovered AND
	// outlined in the same frame (Hover(true) from the OnUpdate loop,
	// Hover(false) from HandleSelectionTrace when the cursor is over
	// them), and the two passes were visibly mixing into a yellow-green
	// blend. Yellow doubles up cleanly. It also avoids the selection
	// outline looking like the green move-gizmo arrow.
	private static readonly Color _selectedColor = Color.Yellow;

	#endregion

	#region Fields

	[Property]
	public Material ModelMaterial;

	protected PolygonMesh _polygonMesh;
	protected Model _model;
	protected MeshComponent _meshComponent;

	protected int _triangleCount;

	private Transform _lastWorldTransform;
	private bool _faceUVsInitialised;
	private bool _isConvex = true;

	#endregion

	#region Properties

	[Property]
	public BooleanOperation Operation { get; set; }

	public ModelBrush OperationBrush { get; set; }

	public Model Model => _model;
	public PolygonMesh PolygonMesh => _polygonMesh;
	public MeshComponent MeshComponent => _meshComponent;
	internal BBox Bounds => _polygonMesh.CalculateBounds();
	public bool IsConvex => _isConvex;

	#endregion

	#region Lifecycle

	protected override void OnAwake()
	{
		base.OnAwake();
		_polygonMesh = new PolygonMesh();
		Build();
		Brushes.Add( this );
	}

	protected override void OnStart()
	{
		base.OnStart();

		if ( _polygonMesh is not null )
		{
			_polygonMesh.SetTransform( GameObject.WorldTransform );
			_lastWorldTransform = GameObject.WorldTransform;
			if ( !_faceUVsInitialised )
			{
				ApplyDefaultFaceUVs();
				_faceUVsInitialised = true;
			}
			RebuildMeshComponent();
		}

		if ( Networking.IsHost && !GameObject.Network.Active )
		{
			GameObject.NetworkSpawn();
		}
	}

	protected override void OnDestroy()
	{
		base.OnDestroy();
		Brushes.Remove( this );
	}

	protected override void OnUpdate()
	{
		SyncMeshWorldTransform();
	}

	[Rpc.Broadcast]
	public void RpcApplyDefaultFaceUVs()
	{
		if ( !this.IsValid() || _polygonMesh is null ) return;
		_polygonMesh.SetTransform( GameObject.WorldTransform );
		_lastWorldTransform = GameObject.WorldTransform;
		ApplyDefaultFaceUVs();
		_faceUVsInitialised = true;
		RebuildMeshComponent();
	}

	private void ApplyDefaultFaceUVs()
	{
		if ( _polygonMesh is null ) return;
		var rotation = WorldRotation;
		foreach ( var face in _polygonMesh.FaceHandles )
		{
			_polygonMesh.ComputeFaceNormal( face, out var localNormal );
			var worldNormal = rotation * localNormal;
			var (axisU, axisV) = DefaultUVAxesForNormal( worldNormal );
			_polygonMesh.SetFaceTextureParameters( face,
				new Vector4( axisU, 0f ),
				new Vector4( axisV, 0f ),
				new Vector2( DefaultUVScale, DefaultUVScale ) );
		}
	}

	public static (Vector3 u, Vector3 v) DefaultUVAxesForNormal( Vector3 normal )
	{
		var ax = MathF.Abs( normal.x );
		var ay = MathF.Abs( normal.y );
		var az = MathF.Abs( normal.z );
		if ( az >= ax && az >= ay )
		{
			return (Vector3.Forward, -Vector3.Left);
		}
		if ( ax >= ay )
		{
			return (Vector3.Left, -Vector3.Up);
		}
		return (Vector3.Forward, -Vector3.Up);
	}

	private void SyncMeshWorldTransform()
	{
		if ( _polygonMesh is null ) return;
		var wt = GameObject.WorldTransform;
		if ( _lastWorldTransform == wt ) return;

		var lockUVs = CanLocalEdit() && (Player.Instance?.LockUVs ?? false);

		_polygonMesh.SetTransform( wt );
		if ( lockUVs )
		{
			// Texture stays glued to the surface: coords don't change, but the
			// world transform did, so the saved per-face params (which live in
			// world-axis space) no longer match. Re-derive them from the
			// preserved coords + new transform — otherwise a later unlocked
			// edit (or an undo snapshot taken now) would later run Compute
			// FaceTextureCoordinatesFromParameters with stale axes and rewrite
			// the texcoords to the wrong place.
			_polygonMesh.ComputeFaceTextureParametersFromCoordinates();
		}
		else
		{
			_polygonMesh.ComputeFaceTextureCoordinatesFromParameters();
		}
		_lastWorldTransform = wt;
		RebuildMeshComponent();
	}

	#endregion

	#region Networking

	public bool CanLocalEdit()
	{
		if ( !GameObject.Network.Active ) return true;
		if ( Networking.IsHost ) return true;
		return GameObject.Network.Owner == Connection.Local;
	}

	// Take network ownership of this brush so subsequent transform writes
	// replicate to peers. CanLocalEdit lets the host modify any brush, but
	// s&box's GameObject transform sync is owner-authoritative — without
	// taking ownership first, WorldPosition / WorldRotation / WorldScale
	// writes from a non-owner get clobbered by the actual owner's state.
	// Mesh edits don't need this (they propagate via RpcApplyMeshSnapshot).
	// No-op when networking is inactive, when we already own the brush,
	// or when we don't have local-edit rights.
	public void TakeOwnershipForEdit()
	{
		if ( !this.IsValid() ) return;
		var go = GameObject;
		if ( go is null ) return;
		var net = go.Network;
		if ( net is null || !net.Active ) return;
		var localConn = Connection.Local;
		if ( net.Owner == localConn ) return;
		if ( !CanLocalEdit() ) return;
		try
		{
			net.AssignOwnership( localConn );
		}
		catch ( Exception e )
		{
			Log.Warning( $"togethercsg: failed to take ownership of '{go.Name}' ({go.Id}) for edit: {e.Message}" );
		}
	}

	[Rpc.Broadcast]
	public void RpcApplyBoolean( ModelBrush source, BooleanOperation op, bool destroySource )
	{
		if ( !this.IsValid() || source is null || !source.IsValid() ) return;
		OperationBrush = source;
		Operation = op;
		ApplyOperators();
		OperationBrush = null;

		if ( destroySource )
		{
			var net = source.GameObject?.Network;
			var canDestroy = net is null || Networking.IsHost || net.Owner == Connection.Local;
			if ( canDestroy ) source.GameObject?.Destroy();
		}
	}

	[Rpc.Broadcast]
	public void RpcCopyMeshFrom( ModelBrush source, bool lockUVs )
	{
		if ( !this.IsValid() || source is null || !source.IsValid() ) return;
		CopyMeshFrom( source, lockUVs );
	}

	[Rpc.Broadcast]
	public void RpcSetFaceMaterial( int triangleIndex, string texturePath )
	{
		if ( !this.IsValid() || _polygonMesh is null ) return;
		if ( triangleIndex < 0 || triangleIndex >= _triangleCount ) return;
		var face = _polygonMesh.TriangleToFace( triangleIndex );

		if ( string.IsNullOrEmpty( texturePath ) )
		{
			_polygonMesh.SetFaceMaterial( face, (Material)null );
			RebuildMeshComponent();
			return;
		}

		GameNetwork.EnsureMaterial( texturePath, material =>
		{
			if ( !this.IsValid() || _polygonMesh is null || material is null ) return;
			_polygonMesh.SetFaceMaterial( face, material );
			_polygonMesh.ComputeFaceTextureParametersFromCoordinates();
			RebuildMeshComponent();
		} );
	}

	[Rpc.Broadcast]
	public void RpcSetAllFaceMaterial( string texturePath )
	{
		if ( !this.IsValid() || _polygonMesh is null ) return;
		if ( string.IsNullOrEmpty( texturePath ) )
		{
			foreach ( var face in _polygonMesh.FaceHandles )
			{
				_polygonMesh.SetFaceMaterial( face, (Material)null );
			}
			RebuildMeshComponent();
			return;
		}

		GameNetwork.EnsureMaterial( texturePath, material =>
		{
			if ( !this.IsValid() || _polygonMesh is null || material is null ) return;
			foreach ( var face in _polygonMesh.FaceHandles )
			{
				_polygonMesh.SetFaceMaterial( face, material );
			}
			_polygonMesh.ComputeFaceTextureParametersFromCoordinates();
			RebuildMeshComponent();
		} );
	}

	[Rpc.Broadcast]
	public void RpcSetFaceUVScaleOffset( int triangleIndex, Vector2 scale, Vector2 offset )
	{
		if ( !this.IsValid() || _polygonMesh is null ) return;
		if ( triangleIndex < 0 || triangleIndex >= _triangleCount ) return;
		var face = _polygonMesh.TriangleToFace( triangleIndex );
		_polygonMesh.SetTextureScale( face, scale );
		_polygonMesh.SetTextureOffset( face, offset );
		var faces = new HashSet<FaceHandle> { face };
		_polygonMesh.ComputeFaceTextureCoordinatesFromParameters( faces );
		RebuildMeshComponent();
	}

	[Rpc.Broadcast]
	public void RpcSetFaceTextureAxes( int triangleIndex, Vector4 axisU, Vector4 axisV, Vector2 scale )
	{
		if ( !this.IsValid() || _polygonMesh is null ) return;
		if ( triangleIndex < 0 || triangleIndex >= _triangleCount ) return;
		var face = _polygonMesh.TriangleToFace( triangleIndex );
		_polygonMesh.SetFaceTextureParameters( face, axisU, axisV, scale );
		var faces = new HashSet<FaceHandle> { face };
		_polygonMesh.ComputeFaceTextureCoordinatesFromParameters( faces );
		RebuildMeshComponent();
	}

	private void RebuildMeshComponent()
	{
		if ( _meshComponent is null ) return;
		_meshComponent.Mesh = _polygonMesh;
		_meshComponent.Enabled = false;
		_meshComponent.RebuildMesh();
		_meshComponent.Enabled = true;
	}

	// Re-runs the subclass's Build() against a fresh PolygonMesh so a
	// change to build-time parameters (e.g. Slices / Stacks on the
	// primitive subclasses) actually takes visual effect after the
	// brush has already been spawned. Mirrors what OnAwake + OnStart
	// would do on a fresh brush: build, apply world transform, set
	// default UVs, refresh the mesh component.
	protected void RegenerateBuildMesh()
	{
		var worldTransform = GameObject.WorldTransform;
		_polygonMesh = new PolygonMesh();
		Build();
		_polygonMesh.SetTransform( worldTransform );
		_lastWorldTransform = worldTransform;
		ApplyDefaultFaceUVs();
		_faceUVsInitialised = true;
		RebuildMeshComponent();
	}

	private void RefreshModelFromPolygonMesh()
	{
		// PolygonMesh.Rebuild() can return null when the mesh has no faces
		// (e.g. a degenerate CSG fragment whose every plane polygon was
		// dropped during RebuildPolygons), and GetIndices() on an otherwise
		// empty model returns null too. NREing here left the brush in a
		// half-replaced state that the user then hit through ApplyOperators.
		// Treat "no model / no indices" as a zero-triangle brush instead.
		_model = _polygonMesh?.Rebuild();
		var indices = _model?.GetIndices();
		_triangleCount = indices is null ? 0 : indices.Length / 3;
	}

	public void ReplacePolygonMesh( PolygonMesh mesh )
	{
		if ( mesh is null ) return;
		_polygonMesh = mesh;
		_polygonMesh.SetTransform( GameObject.WorldTransform );
		_polygonMesh.ComputeFaceTextureCoordinatesFromParameters();
		_lastWorldTransform = GameObject.WorldTransform;
		RefreshModelFromPolygonMesh();
		RefreshConvexState();
		_faceUVsInitialised = true;
		if ( _meshComponent != null )
		{
			RebuildMeshComponent();
		}
	}

	public byte[] SerializeMeshState()
	{
		var faces = _polygonMesh.FaceHandles.ToArray();
		var savedMaterials = new Material[faces.Length];
		var paths = new string[faces.Length];
		for ( var i = 0; i < faces.Length; i++ )
		{
			savedMaterials[i] = _polygonMesh.GetFaceMaterial( faces[i] );
			paths[i] = GameNetwork.GetPathForMaterial( savedMaterials[i] ) ?? "";
			_polygonMesh.SetFaceMaterial( faces[i], (Material)null );
		}

		byte[] meshJson;
		try
		{
			using var jsonStream = new MemoryStream();
			using ( var jw = new Utf8JsonWriter( jsonStream ) )
			{
				PolygonMesh.JsonWrite( _polygonMesh, jw );
			}
			meshJson = jsonStream.ToArray();
		}
		finally
		{
			for ( var i = 0; i < faces.Length; i++ )
			{
				_polygonMesh.SetFaceMaterial( faces[i], savedMaterials[i] );
			}
		}

		using var ms = new MemoryStream();
		using var w = new BinaryWriter( ms );
		w.Write( meshJson.Length );
		w.Write( meshJson );
		w.Write( paths.Length );
		foreach ( var p in paths ) w.Write( p ?? "" );

		w.Write( faces.Length );
		foreach ( var face in faces )
		{
			_polygonMesh.GetFaceTextureParameters( face, out var axisU, out var axisV, out var faceScale );
			var tileScale = _polygonMesh.GetTextureScale( face );
			var offset = _polygonMesh.GetTextureOffset( face );
			w.Write( axisU.x ); w.Write( axisU.y ); w.Write( axisU.z ); w.Write( axisU.w );
			w.Write( axisV.x ); w.Write( axisV.y ); w.Write( axisV.z ); w.Write( axisV.w );
			w.Write( faceScale.x ); w.Write( faceScale.y );
			w.Write( tileScale.x ); w.Write( tileScale.y );
			w.Write( offset.x ); w.Write( offset.y );
		}
		return ms.ToArray();
	}

	public void DeserializeMeshState( byte[] data )
	{
		if ( data is null || data.Length == 0 ) return;
		using var ms = new MemoryStream( data );
		using var r = new BinaryReader( ms );
		var jsonLen = r.ReadInt32();
		var meshJson = r.ReadBytes( jsonLen );

		PolygonMesh dst;
		try
		{
			var jsonReader = new Utf8JsonReader( meshJson );
			jsonReader.Read();
			dst = (PolygonMesh)PolygonMesh.JsonRead( ref jsonReader, typeof( PolygonMesh ) );
		}
		catch ( Exception e )
		{
			Log.Warning( $"togethercsg: snapshot deserialize failed: {e.Message}" );
			return;
		}
		if ( dst is null ) return;

		_polygonMesh = dst;
		_polygonMesh.SetTransform( GameObject.WorldTransform );
		_polygonMesh.ComputeFaceTextureParametersFromCoordinates();
		_lastWorldTransform = GameObject.WorldTransform;
		RefreshModelFromPolygonMesh();
		RefreshConvexState();

		_faceUVsInitialised = true;

		if ( _meshComponent is not null )
		{
			RebuildMeshComponent();
		}

		var faces = _polygonMesh.FaceHandles.ToArray();
		var pathCount = r.ReadInt32();
		for ( var i = 0; i < pathCount && i < faces.Length; i++ )
		{
			var path = r.ReadString();
			if ( string.IsNullOrEmpty( path ) ) continue;
			var face = faces[i];
			GameNetwork.EnsureMaterial( path, mat =>
			{
				if ( !this.IsValid() || _polygonMesh is null || mat is null ) return;
				_polygonMesh.SetFaceMaterial( face, mat );
				RebuildMeshComponent();
			} );
		}

		if ( ms.Position >= ms.Length ) return;
		int paramCount;
		try { paramCount = r.ReadInt32(); }
		catch ( EndOfStreamException )
		{
			// Trailing per-face UV-param block is optional (older snapshots
			// don't carry it). The earlier Position/Length check covers the
			// common case; this is the truncation-mid-int safety net.
			return;
		}
		var paramsApplied = false;
		for ( var i = 0; i < paramCount && i < faces.Length; i++ )
		{
			var axisU = new Vector4( r.ReadSingle(), r.ReadSingle(), r.ReadSingle(), r.ReadSingle() );
			var axisV = new Vector4( r.ReadSingle(), r.ReadSingle(), r.ReadSingle(), r.ReadSingle() );
			var faceScale = new Vector2( r.ReadSingle(), r.ReadSingle() );
			var tileScale = new Vector2( r.ReadSingle(), r.ReadSingle() );
			var offset = new Vector2( r.ReadSingle(), r.ReadSingle() );
			var face = faces[i];
			_polygonMesh.SetFaceTextureParameters( face, axisU, axisV, faceScale );
			_polygonMesh.SetTextureScale( face, tileScale );
			_polygonMesh.SetTextureOffset( face, offset );
			paramsApplied = true;
		}
		if ( paramsApplied )
		{
			_polygonMesh.ComputeFaceTextureCoordinatesFromParameters();
			RebuildMeshComponent();
		}
	}

	[Rpc.Broadcast]
	public void RpcApplyMeshSnapshot( byte[] data )
	{
		if ( !this.IsValid() ) return;
		DeserializeMeshState( data );
	}

	#endregion

	#region Build / Mesh

	protected virtual void Build()
	{
		_model = _polygonMesh.Rebuild();
		_polygonMesh.ComputeFaceTextureParametersFromCoordinates();
		_meshComponent = GameObject.GetComponent<MeshComponent>() ?? AddComponent<MeshComponent>();
		_meshComponent.Collision = MeshComponent.CollisionType.Hull;
		_meshComponent.Mesh = _polygonMesh;
		RefreshConvexState();
	}

	public void CopyMeshFrom( ModelBrush source, bool lockUVs )
	{
		if ( source is null || source._polygonMesh is null ) return;

		var src = source._polygonMesh;
		var dst = new PolygonMesh();
		// Pin the destination mesh to the clone's world transform up front
		// so the engine resolves per-face axis/offset against the clone's
		// world space when we either derive params from coords (lock on) or
		// derive coords from params (lock off).
		dst.SetTransform( GameObject.WorldTransform );

		var vertMap = new Dictionary<int, VertexHandle>();

		foreach ( var srcFace in src.FaceHandles )
		{
			var srcVerts = src.GetFaceVertices( srcFace );
			var newVerts = new VertexHandle[srcVerts.Length];
			for ( var i = 0; i < srcVerts.Length; i++ )
			{
				var v = srcVerts[i];
				if ( !vertMap.TryGetValue( v.Index, out var newVert ) )
				{
					newVert = dst.AddVertex( src.GetVertexPosition( v ) );
					vertMap[v.Index] = newVert;
				}
				newVerts[i] = newVert;
			}

			var newFace = dst.AddFace( newVerts );

			var mat = src.GetFaceMaterial( srcFace );
			if ( mat != null ) dst.SetFaceMaterial( newFace, mat );

			// Always copy per-face params (axes are in world-axis space, so
			// a same-rotation clone reuses them directly). Without this the
			// lock-off path below would compute coords against zeroed axes.
			src.GetFaceTextureParameters( srcFace, out var axisU, out var axisV, out var faceScale );
			dst.SetFaceTextureParameters( newFace, axisU, axisV, faceScale );
			dst.SetTextureScale( newFace, src.GetTextureScale( srcFace ) );
			dst.SetTextureOffset( newFace, src.GetTextureOffset( srcFace ) );

			if ( lockUVs )
			{
				// Texture glued to the surface: reproduce the source's
				// face-vertex coords on the clone regardless of the clone's
				// world-space offset. The copied coords are authoritative;
				// params get re-derived to match them under the clone's
				// transform below.
				var coords = src.GetFaceTextureCoords( srcFace );
				if ( coords is not null && coords.Length > 0 )
				{
					dst.SetFaceTextureCoords( newFace, coords );
				}
			}
		}

		_polygonMesh = dst;
		if ( lockUVs )
		{
			_polygonMesh.ComputeFaceTextureParametersFromCoordinates();
		}
		else
		{
			// Texture anchored in world space: derive coords from the copied
			// world-axis params under the clone's transform. The clone's
			// offset shifts the resulting coords relative to the source —
			// that's the intended look for unlocked UVs and matches what
			// happens when you move a single brush with the lock off.
			_polygonMesh.ComputeFaceTextureCoordinatesFromParameters();
		}
		_lastWorldTransform = GameObject.WorldTransform;
		RefreshModelFromPolygonMesh();
		RefreshConvexState();

		_faceUVsInitialised = true;

		if ( _meshComponent != null )
		{
			RebuildMeshComponent();
		}
	}

	private static PolygonMesh CloneMeshWithPointTransform( PolygonMesh source, Func<Vector3, Vector3> transformPoint, float inflateEpsilon = 0f )
	{
		var dst = new PolygonMesh();
		var vertMap = new Dictionary<int, VertexHandle>();

		var doInflate = inflateEpsilon > 0f;
		var center = Vector3.Zero;

		if ( doInflate )
		{
			var min = new Vector3( float.PositiveInfinity );
			var max = new Vector3( float.NegativeInfinity );
			var any = false;
			foreach ( var srcFace in source.FaceHandles )
			{
				foreach ( var v in source.GetFaceVertices( srcFace ) )
				{
					var p = transformPoint( source.GetVertexPosition( v ) );
					min = Vector3.Min( min, p );
					max = Vector3.Max( max, p );
					any = true;
				}
			}

			if ( any )
				center = (min + max) * 0.5f;
			else
				doInflate = false;
		}

		foreach ( var srcFace in source.FaceHandles )
		{
			var srcVerts = source.GetFaceVertices( srcFace );
			var newVerts = new VertexHandle[srcVerts.Length];
			for ( var i = 0; i < srcVerts.Length; i++ )
			{
				var v = srcVerts[i];
				if ( !vertMap.TryGetValue( v.Index, out var newVert ) )
				{
					var p = transformPoint( source.GetVertexPosition( v ) );
					if ( doInflate )
					{
						p.x += inflateEpsilon * MathF.Sign( p.x - center.x );
						p.y += inflateEpsilon * MathF.Sign( p.y - center.y );
						p.z += inflateEpsilon * MathF.Sign( p.z - center.z );
					}
					newVert = dst.AddVertex( p );
					vertMap[v.Index] = newVert;
				}
				newVerts[i] = newVert;
			}

			var newFace = dst.AddFace( newVerts );
			var mat = source.GetFaceMaterial( srcFace );
			if ( mat != null ) dst.SetFaceMaterial( newFace, mat );

			source.GetFaceTextureParameters( srcFace, out var axisU, out var axisV, out var scale );
			dst.SetFaceTextureParameters( newFace, axisU, axisV, scale );
			dst.SetTextureScale( newFace, source.GetTextureScale( srcFace ) );
			dst.SetTextureOffset( newFace, source.GetTextureOffset( srcFace ) );
		}

		return dst;
	}

	// TrenchBroom-style convexity check: for every face plane, no vertex
	// may lie strictly on the outside. A convex polyhedron is the
	// intersection of its face half-spaces, so any vertex poking past a
	// face's plane proves concavity at that face. O(F * V), called
	// whenever the mesh changes — cached on `_isConvex` and surfaced via
	// the `IsConvex` property.
	//
	// TrenchBroom can rely on `plane_status::above` directly because its
	// half-edge polyhedron is built with outward-pointing normals as an
	// invariant. Our PolygonMesh has no such guarantee — `ComputeFaceNormal`
	// can come back inward for CSG results, MAP imports, and slice cap
	// faces. We fix this by flipping each face normal to point away from
	// the mesh centroid before testing, so "above the plane" reliably
	// means "outside the brush". The centroid of a convex mesh's vertices
	// always sits inside, so the flip is unambiguous for any mesh that
	// could plausibly pass.
	private static bool IsConvexClosedMesh( PolygonMesh mesh )
	{
		if ( mesh is null ) return false;
		var faces = mesh.FaceHandles.ToArray();
		if ( faces.Length < 4 ) return false;
		var vertices = mesh.VertexHandles.ToArray();
		if ( vertices.Length < 4 ) return false;

		var meshCentroid = Vector3.Zero;
		foreach ( var v in vertices ) meshCentroid += mesh.GetVertexPosition( v );
		meshCentroid /= vertices.Length;

		foreach ( var face in faces )
		{
			var verts = mesh.GetFaceVertices( face );
			if ( verts.Length < 3 ) return false;
			mesh.ComputeFaceNormal( face, out var normal );
			if ( normal.LengthSquared < 1e-12f ) return false;
			normal = normal.Normal;

			var faceOrigin = mesh.GetVertexPosition( verts[0] );
			if ( Vector3.Dot( normal, faceOrigin - meshCentroid ) < 0f ) normal = -normal;

			foreach ( var v in vertices )
			{
				var d = Vector3.Dot( normal, mesh.GetVertexPosition( v ) - faceOrigin );
				if ( d > ConvexityEpsilon ) return false;
			}
		}
		return true;
	}

	private void RefreshConvexState()
	{
		_isConvex = IsConvexClosedMesh( _polygonMesh );
	}

	// Draws every face outline in the given color, like HoverOutline, used
	// to flag a brush as concave/invalid for operators while
	// EXPERIMENTAL_BRUSH_CSG is set.
	public void DrawConcaveOutline( Color color )
	{
		if ( _polygonMesh is null ) return;
		foreach ( var face in _polygonMesh.FaceHandles )
		{
			var vertices = _polygonMesh.GetFaceVertices( face );
			for ( var i = 0; i < vertices.Length; i++ )
			{
				var a = WorldTransform.PointToWorld( _polygonMesh.GetVertexPosition( vertices[i] ) );
				var b = WorldTransform.PointToWorld( _polygonMesh.GetVertexPosition( vertices[(i + 1) % vertices.Length] ) );
				CustomOverlay.Line( a, b, color, 0f, default, true, Player.OutlineLineThickness );
			}
		}
	}

	public void ApplyOperators()
	{
		var aWorld = GameObject.WorldTransform;
		var bWorld = OperationBrush.GameObject.WorldTransform;

		var bInflate = Operation switch
		{
			BooleanOperation.Subtract => BooleanCoplanarEpsilon,
			BooleanOperation.Union => BooleanCoplanarEpsilon,
			BooleanOperation.Intersect => BooleanCoplanarEpsilon,
			_ => 0f
		};

		var aLocal = CloneMeshWithPointTransform( _polygonMesh, v => aWorld.PointToLocal( aWorld.PointToWorld( v ) ) );
		var bInALocal = CloneMeshWithPointTransform( OperationBrush._polygonMesh, v => aWorld.PointToLocal( bWorld.PointToWorld( v ) ), bInflate );

		// Engine's PerformBoolean re-computes face-vertex UVs inside each
		// ProcessOperand call using `this.Transform.PointToWorld(vertex)` as
		// the world position fed into `dot(axisU, world_pos)`. The axisU
		// values copied off the source brushes are in WORLD space (derived
		// against the source's world transform); if `aLocal.Transform` is
		// left at its default identity, the recompute dots those world-
		// space axes against local-space positions and produces garbage
		// UVs. Pin the work mesh to A's world transform so the recompute
		// recovers each operand's original world position (B's vertices
		// were brought into A-local on purpose precisely so that this
		// PointToWorld round-trips them back to B's original world).
		aLocal.SetTransform( aWorld );

		// Union always goes through the mesh-CSG path, even with
		// EXPERIMENTAL_BRUSH_CSG on. Brush-CSG's Union returns a brush list
		// (Subtract(A,B) + B appended) — the editor surfaces extra brushes
		// as separate ModelBrush GameObjects, which looked to users like
		// "the union did nothing". Subtract and Intersect still benefit
		// from the brush-CSG path since their outputs map cleanly to one
		// or more fragment brushes.
		var useBrushCsg = false;
#if EXPERIMENTAL_BRUSH_CSG
		useBrushCsg = Operation != BooleanOperation.Union;
#endif

		if ( useBrushCsg )
		{
#if EXPERIMENTAL_BRUSH_CSG
			// Brush-CSG path. ConvexBrush.FromConvexPolygonMesh treats the input
			// as the intersection of its face half-spaces and rebuilds each face
			// polygon from triple plane intersections — so a non-planar or
			// non-convex operand would get silently snapped to a different shape
			// rather than performing the user's edit. Refuse the operation
			// instead; the brush is flagged concave in the UI so the user already
			// knows operators don't apply to it.
			if ( !IsConvexClosedMesh( aLocal ) || !IsConvexClosedMesh( bInALocal ) ) return;

			var brushesA = new[] { ConvexBrush.FromConvexPolygonMesh( aLocal ) };
			var brushesB = new[] { ConvexBrush.FromConvexPolygonMesh( bInALocal ) };
			var resultBrushes = ConvexBrush.PerformBoolean( brushesA, brushesB, Operation );
			if ( resultBrushes.Count == 0 ) return;

			_polygonMesh = ConvexBrush.ToPolygonMesh( new[] { resultBrushes[0] } );

			// Each remaining convex piece becomes its own ModelBrush GameObject.
			// Only host spawns the networked fragment objects; clients receive them
			// via NetworkSpawn replication and pick up geometry from the broadcast
			// RpcApplyMeshSnapshot below. In single-player Networking.IsActive is
			// false and we just create local-only fragments here.
			if ( resultBrushes.Count > 1 && (Networking.IsHost || !Networking.IsActive) )
			{
				for ( int i = 1; i < resultBrushes.Count; i++ )
				{
					var fragMesh = ConvexBrush.ToPolygonMesh( new[] { resultBrushes[i] } );
					// Skip empty fragments — ToPolygonMesh can return a face-less
					// mesh when every plane polygon got dropped during
					// RebuildPolygons (degenerate / sliver convex piece). The
					// downstream RefreshModelFromPolygonMesh used to NRE on
					// _model.GetIndices() in that case before we hardened it,
					// but there's also no useful GameObject to spawn for a
					// zero-face brush.
					if ( !fragMesh.FaceHandles.Any() ) continue;

					var fragGo = new GameObject();
					fragGo.Name = $"{GameObject.Name}_frag";
					fragGo.WorldPosition = GameObject.WorldPosition;
					fragGo.WorldRotation = GameObject.WorldRotation;
					fragGo.WorldScale = GameObject.WorldScale;

					var fragBrush = fragGo.Components.Create<ModelBrush>();
					fragBrush.ModelMaterial = ModelMaterial;
					fragBrush.ReplacePolygonMesh( fragMesh );

					if ( Networking.IsActive )
					{
						fragGo.NetworkSpawn();
						// Fragments inherit the destination brush's owner so the peer
						// that initiated the carve can still edit/delete its own
						// pieces — NetworkSpawn() defaults to the caller (always the
						// host here), which would otherwise lock peers out.
						try
						{
							var destNet = GameObject?.Network;
							if ( destNet is { Active: true } && destNet.Owner is not null )
							{
								fragGo.Network.AssignOwnership( destNet.Owner );
							}
						}
						catch ( Exception e )
						{
							Log.Warning( $"togethercsg: fragment ownership transfer failed: {e.Message}" );
						}
						// Replicate geometry to clients. SerializeMeshState handles the
						// PolygonMesh-cycle-safe JSON via PolygonMesh.JsonWrite plus
						// material-path remapping; RpcApplyMeshSnapshot is queued by
						// Sandbox until the spawn lands on the receiver.
						var snapshot = fragBrush.SerializeMeshState();
						fragBrush.RpcApplyMeshSnapshot( snapshot );
					}
				}
			}
#endif
		}
		else
		{
			if ( !aLocal.PerformBoolean( bInALocal, global::Transform.Zero, Operation ) )
			{
				return;
			}

			_polygonMesh = aLocal;

			// The engine's PerformBoolean can leave coincident-but-disconnected
			// vertices along the cut boundary when B's face plane runs along an
			// existing edge of A — same failure mode as slicing through an
			// existing edge: any cap synthesised on that boundary loses its
			// closed loop of shared edges and falls apart. Weld the result so
			// the mesh stays manifold for downstream ops (further slices,
			// chained booleans).
			var verts = _polygonMesh.VertexHandles.ToList();
			if ( verts.Count >= 2 )
			{
				_polygonMesh.MergeVerticesWithinDistance( verts, BooleanWeldEpsilon, bPreConnect: false, bAveragePositions: true, out _ );
			}
		}
		_polygonMesh.SetTransform( aWorld );
		_polygonMesh.ComputeFaceTextureCoordinatesFromParameters();
		_lastWorldTransform = aWorld;
		RefreshModelFromPolygonMesh();
		RebuildMeshComponent();
		RefreshConvexState();
	}

	#endregion

	#region Hover

	private void DrawFaceOutline( FaceHandle face, bool selected )
	{
		var vertices = _polygonMesh.GetFaceVertices( face );
		Vector3 a;
		Vector3 b;
		for ( var i = 0; i < vertices.Length - 1; i++ )
		{
			a = WorldTransform.PointToWorld( _polygonMesh.GetVertexPosition( vertices[i + 0] ) );
			b = WorldTransform.PointToWorld( _polygonMesh.GetVertexPosition( vertices[i + 1] ) );
			CustomOverlay.Line( a, b, _hoverColor, 0f, default, true, Player.OutlineLineThickness );
		}
		a = WorldTransform.PointToWorld( _polygonMesh.GetVertexPosition( vertices[0] ) );
		b = WorldTransform.PointToWorld( _polygonMesh.GetVertexPosition( vertices[vertices.Length - 1] ) );
		CustomOverlay.Line( a, b, selected ? _selectedColor : _hoverColor, 0f, default, true, Player.OutlineLineThickness );
	}

	public void Hover( bool selected = false )
	{
		foreach ( var face in _polygonMesh.FaceHandles )
		{
			DrawFaceOutline( face, selected );
		}
	}

	public void HoverOutline( Color color )
	{
		foreach ( var face in _polygonMesh.FaceHandles )
		{
			var vertices = _polygonMesh.GetFaceVertices( face );
			for ( var i = 0; i < vertices.Length; i++ )
			{
				var a = WorldTransform.PointToWorld( _polygonMesh.GetVertexPosition( vertices[i] ) );
				var b = WorldTransform.PointToWorld( _polygonMesh.GetVertexPosition( vertices[(i + 1) % vertices.Length] ) );
				CustomOverlay.Line( a, b, color, 0f, default, true, Player.OutlineLineThickness );
			}
		}
	}

	public void HoverFace( int triangle, bool selected = false )
	{
		var face = _polygonMesh.TriangleToFace( triangle );
		DrawFaceOutline( face, selected );
	}

	public void HoverEdge( int triangle, uint edgeIndex, bool selected = false )
	{
		var face = _polygonMesh.TriangleToFace( triangle );
		var vertices = _model.GetVertices();
		var indices = _model.GetIndices();
		var i0 = indices[triangle * 3 + 0];
		var i1 = indices[triangle * 3 + 1];
		var i2 = indices[triangle * 3 + 2];
		uint a, b;
		switch ( edgeIndex )
		{
			case 0: a = i1; b = i2; break;
			case 1: a = i2; b = i0; break;
			default: a = i0; b = i1; break;
		}
		var pa = vertices[a].Position;
		var pb = vertices[b].Position;
		var faceVertices = _polygonMesh.GetFaceVertices( face );
		for ( var i = 0; i < faceVertices.Length; i++ )
		{
			var qa = _polygonMesh.GetVertexPosition( faceVertices[i] );
			var qb = _polygonMesh.GetVertexPosition( faceVertices[(i + 1) % faceVertices.Length] );
			var match = (qa.AlmostEqual( pa, VertexMatchEpsilon ) && qb.AlmostEqual( pb, VertexMatchEpsilon ))
				|| (qa.AlmostEqual( pb, VertexMatchEpsilon ) && qb.AlmostEqual( pa, VertexMatchEpsilon ));
			if ( match )
			{
				CustomOverlay.Line(
					WorldTransform.PointToWorld( qa ),
					WorldTransform.PointToWorld( qb ),
					selected ? _selectedColor : _hoverColor, 0f, default, true, Player.OutlineLineThickness );
				return;
			}
		}
	}

	public void HoverVertex( int triangle, int triangleVertexIndex, bool selected = false )
	{
		var face = _polygonMesh.TriangleToFace( triangle );
		var faceVertices = _polygonMesh.GetFaceVertices( face );
		var indices = _model.GetIndices();
		var modelVerts = _model.GetVertices();
		var pos = modelVerts[indices[triangle * 3 + triangleVertexIndex]];
		foreach ( var faceVertex in faceVertices )
		{
			if ( _polygonMesh.GetVertexPosition( faceVertex ).AlmostEqual( pos.Position, VertexMatchEpsilon ) )
			{
				DrawVertexGizmoBox( _polygonMesh.GetVertexPosition( faceVertex ), selected );
				return;
			}
		}
	}

	public void HoverVertexByIndex( int vertexIndex, bool selected = false )
	{
		if ( _polygonMesh is null ) return;
		foreach ( var v in _polygonMesh.VertexHandles )
		{
			if ( v.Index != vertexIndex ) continue;
			DrawVertexGizmoBox( _polygonMesh.GetVertexPosition( v ), selected );
			return;
		}
	}

	// Match the move/rotate gizmos' camera-distance falloff so vertex pick
	// boxes stay roughly screen-constant — without this they shrink to nothing
	// when zoomed out and balloon when zoomed in.
	private void DrawVertexGizmoBox( Vector3 localPos, bool selected )
	{
		var worldPos = WorldTransform.PointToWorld( localPos );
		var camera = Scene?.Camera;
		var scale = 1f;
		if ( camera is not null )
		{
			var distance = Vector3.DistanceBetween( camera.WorldPosition, worldPos );
			scale = MathF.Max( Player.GizmoMinScale, distance / Player.GizmoReferenceDistance );
		}
		CustomOverlay.Box(
			worldPos,
			Vector3.One * VertexGizmoSize * scale,
			selected ? _selectedColor : _hoverColor, 0f, default, true, Player.OutlineLineThickness );
	}

	public bool TryGetVertexIndexAtTriangle( int triangle, int triangleVertexIndex, out int vertexIndex )
	{
		vertexIndex = -1;
		if ( _polygonMesh is null || _model is null ) return false;
		var face = _polygonMesh.TriangleToFace( triangle );
		var faceVertices = _polygonMesh.GetFaceVertices( face );
		var indices = _model.GetIndices();
		var modelVerts = _model.GetVertices();
		var triBase = triangle * 3 + triangleVertexIndex;
		if ( triBase < 0 || triBase >= indices.Length ) return false;
		var pos = modelVerts[indices[triBase]].Position;

		// `pos` lives in the model's coordinate space — usually local, but
		// PolygonMesh.Rebuild may bake the world transform set by SetTransform
		// after operations like ApplyOperators or MoveVerticesLocal. The
		// polygon-mesh face vertices are always local, so for a rotated brush
		// the two ended up in different spaces and the equality test never
		// matched. Take the closest face vertex over both candidate spaces so
		// either layout picks the right vertex.
		var bestDistSq = float.PositiveInfinity;
		foreach ( var faceVertex in faceVertices )
		{
			var local = _polygonMesh.GetVertexPosition( faceVertex );
			var world = WorldTransform.PointToWorld( local );
			var dSq = MathF.Min( (local - pos).LengthSquared, (world - pos).LengthSquared );
			if ( dSq >= bestDistSq ) continue;
			bestDistSq = dSq;
			vertexIndex = faceVertex.Index;
		}
		// Generous tolerance since rotated-and-scaled world-space matches can
		// drift well past the strict local-space epsilon. Still small relative
		// to typical inter-vertex spacing so we don't snap onto the wrong one.
		var tol = VertexMatchEpsilon * 10f;
		if ( bestDistSq <= tol * tol ) return true;
		vertexIndex = -1;
		return false;
	}

	public bool TryGetVertexLocalPosition( int vertexIndex, out Vector3 local )
	{
		if ( _polygonMesh is not null )
		{
			foreach ( var v in _polygonMesh.VertexHandles )
			{
				if ( v.Index != vertexIndex ) continue;
				local = _polygonMesh.GetVertexPosition( v );
				return true;
			}
		}
		local = default;
		return false;
	}

	public bool TryGetVertexWorldPosition( int vertexIndex, out Vector3 world )
	{
		if ( TryGetVertexLocalPosition( vertexIndex, out var local ) )
		{
			world = WorldTransform.PointToWorld( local );
			return true;
		}
		world = default;
		return false;
	}

	// Rebuild the polygon mesh with the given vertices moved to new local
	// positions. PolygonMesh doesn't expose a SetVertexPosition, so we copy
	// face-by-face while substituting new positions for the moved indices.
	// Returns oldIndex -> newIndex so callers can keep their selection /
	// drag-tracking maps in sync, because the rebuild reassigns VertexHandle
	// indices in the order vertices are first walked.
	public Dictionary<int, int> MoveVerticesLocal( IReadOnlyDictionary<int, Vector3> moves )
	{
		if ( _polygonMesh is null || moves is null || moves.Count == 0 ) return null;

		var src = _polygonMesh;
		var dst = new PolygonMesh();
		dst.SetTransform( GameObject.WorldTransform );

		// LockUVs on: texture is glued to the surface as the surface deforms,
		// so we copy per-vertex texcoords across face-by-face and re-derive
		// the axis/scale/offset params from those coords once the new mesh
		// is built. LockUVs off: the texture is anchored in world space, so
		// we keep the params and let ComputeFaceTextureCoordinatesFromParameters
		// derive new coords from the moved-vertex world positions.
		var lockUVs = CanLocalEdit() && (Player.Instance?.LockUVs ?? false);

		var vertMap = new Dictionary<int, VertexHandle>();
		var remap = new Dictionary<int, int>();

		foreach ( var srcFace in src.FaceHandles )
		{
			var srcVerts = src.GetFaceVertices( srcFace );
			var newVerts = new VertexHandle[srcVerts.Length];
			for ( var i = 0; i < srcVerts.Length; i++ )
			{
				var v = srcVerts[i];
				if ( !vertMap.TryGetValue( v.Index, out var newVert ) )
				{
					var pos = moves.TryGetValue( v.Index, out var moved )
						? moved
						: src.GetVertexPosition( v );
					newVert = dst.AddVertex( pos );
					vertMap[v.Index] = newVert;
					remap[v.Index] = newVert.Index;
				}
				newVerts[i] = newVert;
			}

			var newFace = dst.AddFace( newVerts );

			var mat = src.GetFaceMaterial( srcFace );
			if ( mat != null ) dst.SetFaceMaterial( newFace, mat );

			src.GetFaceTextureParameters( srcFace, out var axisU, out var axisV, out var scale );
			dst.SetFaceTextureParameters( newFace, axisU, axisV, scale );
			dst.SetTextureScale( newFace, src.GetTextureScale( srcFace ) );
			dst.SetTextureOffset( newFace, src.GetTextureOffset( srcFace ) );

			if ( lockUVs )
			{
				var coords = src.GetFaceTextureCoords( srcFace );
				if ( coords is not null && coords.Length == newVerts.Length )
				{
					dst.SetFaceTextureCoords( newFace, coords );
				}
			}
		}

		_polygonMesh = dst;
		if ( lockUVs )
		{
			// Coords are authoritative; refresh params so a later unlocked
			// edit doesn't recompute coords from stale axes (same reasoning
			// as SyncMeshWorldTransform's locked branch).
			_polygonMesh.ComputeFaceTextureParametersFromCoordinates();
		}
		else
		{
			_polygonMesh.ComputeFaceTextureCoordinatesFromParameters();
		}
		_lastWorldTransform = GameObject.WorldTransform;
		RefreshModelFromPolygonMesh();
		RefreshConvexState();
		if ( _meshComponent is not null ) RebuildMeshComponent();
		return remap;
	}

	#endregion
}
