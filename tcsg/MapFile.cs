using HalfEdgeMesh;
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;
using static Sandbox.PolygonMesh;

// Quake-style .map file reader and writer (Valve 220 brush format). Each brush
// is defined as the convex intersection of half-spaces given as 3 points per
// face; we rebuild explicit polygon geometry by intersecting plane triples and
// keeping the points that satisfy every half-space, then sort each face's
// points around the outward normal so PolygonMesh receives properly wound
// polygons.
public static class MapFile
{
	#region Constants

	private const float PlaneEpsilon = 0.01f;
	private const float VertexMergeEpsilon = 0.01f;
	private const float DegenerateDenomEpsilon = 1e-6f;
	// Two face normals dot above this (effectively > 1 - 1e-4) are treated
	// as the same direction for coplanar-face deduplication on export.
	private const float CoplanarNormalEpsilon = 1e-4f;
	private const float DefaultExportUVScale = 1f;
	// Quake-default texture size used when the real texture's dimensions
	// aren't known yet (failed load, missing material, etc).
	private const float FallbackTextureSize = 64f;
	private const string EmptyTextureName = "__TB_empty";

	public static (float w, float h) DefaultTextureSize( float w, float h ) =>
		(w > 0f ? w : FallbackTextureSize, h > 0f ? h : FallbackTextureSize);

	#endregion

	#region Public Types

	public sealed class Face
	{
		public Vector3 P0;
		public Vector3 P1;
		public Vector3 P2;
		public string Texture;
		// True when the face line carried explicit `[ ux uy uz uoff ]` axes
		// (Valve 220 format). False for the classic Quake format that only has
		// 5 numbers (xOff yOff rot xScale yScale); base axes are derived from
		// the face normal at UV time in that case.
		public bool HasExplicitAxes;
		public Vector3 UAxis;
		public float UOffset;
		public Vector3 VAxis;
		public float VOffset;
		public float Rotation;
		public float UScale;
		public float VScale;

		public Vector3 PlaneNormal
		{
			get
			{
				var n = Vector3.Cross( P2 - P0, P1 - P0 );
				var len = n.Length;
				return len > DegenerateDenomEpsilon ? n / len : Vector3.Zero;
			}
		}

		public float PlaneDistance => Vector3.Dot( PlaneNormal, P0 );
	}

	public sealed class Brush
	{
		public List<Face> Faces { get; } = new();
	}

	public sealed class Entity
	{
		public string ClassName = "worldspawn";
		public Dictionary<string, string> Properties { get; } = new( StringComparer.OrdinalIgnoreCase );
		public List<Brush> Brushes { get; } = new();
	}

	public sealed class MapData
	{
		public List<Entity> Entities { get; } = new();
	}

	#endregion

	#region Parse

	public static MapData Parse( string text )
	{
		var data = new MapData();
		if ( string.IsNullOrEmpty( text ) ) return data;

		Entity entity = null;
		Brush brush = null;
		var depth = 0;

		// Walk the source as a span, slicing one line at a time and only
		// materialising a string when we actually need to feed downstream
		// parsers that take string. This avoids the array allocation that
		// string.Split would produce, and skips comment/blank lines without
		// touching the heap.
		var source = text.AsSpan();
		while ( source.Length > 0 )
		{
			var newlineIndex = source.IndexOfAny( '\n', '\r' );
			ReadOnlySpan<char> line;
			if ( newlineIndex < 0 )
			{
				line = source;
				source = default;
			}
			else
			{
				line = source[..newlineIndex];
				var skip = newlineIndex + 1;
				// Treat \r\n as a single line break.
				if ( source[newlineIndex] == '\r' && skip < source.Length && source[skip] == '\n' )
				{
					skip++;
				}
				source = source[skip..];
			}

			var trimmed = line.Trim();
			if ( trimmed.Length == 0 ) continue;
			if ( trimmed.Length >= 2 && trimmed[0] == '/' && trimmed[1] == '/' ) continue;

			if ( trimmed.Length == 1 && trimmed[0] == '{' )
			{
				depth++;
				if ( depth == 1 ) entity = new Entity();
				else if ( depth == 2 ) brush = new Brush();
				continue;
			}
			if ( trimmed.Length == 1 && trimmed[0] == '}' )
			{
				if ( depth == 2 )
				{
					if ( brush is not null && brush.Faces.Count >= 4 )
					{
						entity.Brushes.Add( brush );
					}
					brush = null;
				}
				else if ( depth == 1 )
				{
					if ( entity is not null ) data.Entities.Add( entity );
					entity = null;
				}
				depth = Math.Max( 0, depth - 1 );
				continue;
			}

			if ( depth == 2 && brush is not null && trimmed[0] == '(' )
			{
				if ( TryParseFace( trimmed.ToString(), out var face ) )
				{
					brush.Faces.Add( face );
				}
				continue;
			}

			if ( depth == 1 && entity is not null && trimmed[0] == '"' )
			{
				if ( TryParseKeyValue( trimmed.ToString(), out var key, out var value ) )
				{
					entity.Properties[key] = value;
					if ( key.Equals( "classname", StringComparison.OrdinalIgnoreCase ) )
					{
						entity.ClassName = value;
					}
				}
			}
		}

		return data;
	}

	// Entity property line shape: `"key" "value"`. Manual parse avoids pulling
	// in System.Text.RegularExpressions, which is not on the s&box whitelist.
	private static bool TryParseKeyValue( string line, out string key, out string value )
	{
		key = null;
		value = null;
		if ( line.Length < 4 || line[0] != '"' ) return false;
		var endKey = line.IndexOf( '"', 1 );
		if ( endKey < 0 ) return false;
		var startVal = line.IndexOf( '"', endKey + 1 );
		if ( startVal < 0 ) return false;
		var endVal = line.IndexOf( '"', startVal + 1 );
		if ( endVal < 0 ) return false;
		key = line.Substring( 1, endKey - 1 );
		value = line.Substring( startVal + 1, endVal - startVal - 1 );
		return true;
	}

	private static bool TryParseFace( string line, out Face face )
	{
		face = null;
		var tokens = Tokenize( line );
		// 15 tokens for the three points + 1 texture name + 5 minimum for the
		// classic format (or 16 for Valve 220 with bracketed axes).
		if ( tokens.Count < 21 ) return false;

		try
		{
			var i = 0;
			if ( tokens[i++] != "(" ) return false;
			var p0 = ReadVector3( tokens, ref i );
			if ( tokens[i++] != ")" ) return false;
			if ( tokens[i++] != "(" ) return false;
			var p1 = ReadVector3( tokens, ref i );
			if ( tokens[i++] != ")" ) return false;
			if ( tokens[i++] != "(" ) return false;
			var p2 = ReadVector3( tokens, ref i );
			if ( tokens[i++] != ")" ) return false;

			var texture = tokens[i++];

			Vector3 uAxis, vAxis;
			float uOffset, vOffset, rot, uScale, vScale;
			bool hasExplicit;

			if ( i < tokens.Count && tokens[i] == "[" )
			{
				// Valve 220: `[ ux uy uz uoff ] [ vx vy vz voff ] rot uscale vscale`
				hasExplicit = true;
				i++;
				uAxis = ReadVector3( tokens, ref i );
				uOffset = ParseFloat( tokens[i++] );
				if ( tokens[i++] != "]" ) return false;
				if ( tokens[i++] != "[" ) return false;
				vAxis = ReadVector3( tokens, ref i );
				vOffset = ParseFloat( tokens[i++] );
				if ( tokens[i++] != "]" ) return false;
				rot = ParseFloat( tokens[i++] );
				uScale = ParseFloat( tokens[i++] );
				vScale = ParseFloat( tokens[i++] );
			}
			else
			{
				// Classic Quake: `xOffset yOffset rotation xScale yScale`. Base
				// axes get computed later from the face normal.
				hasExplicit = false;
				uAxis = Vector3.Zero;
				vAxis = Vector3.Zero;
				uOffset = ParseFloat( tokens[i++] );
				vOffset = ParseFloat( tokens[i++] );
				rot = ParseFloat( tokens[i++] );
				uScale = ParseFloat( tokens[i++] );
				vScale = ParseFloat( tokens[i++] );
			}

			face = new Face
			{
				P0 = p0,
				P1 = p1,
				P2 = p2,
				Texture = texture,
				HasExplicitAxes = hasExplicit,
				UAxis = uAxis,
				UOffset = uOffset,
				VAxis = vAxis,
				VOffset = vOffset,
				Rotation = rot,
				UScale = uScale == 0f ? 1f : uScale,
				VScale = vScale == 0f ? 1f : vScale,
			};
			return true;
		}
		catch
		{
			face = null;
			return false;
		}
	}

	private static List<string> Tokenize( string line )
	{
		var result = new List<string>();
		var sb = new StringBuilder();
		void Flush()
		{
			if ( sb.Length == 0 ) return;
			result.Add( sb.ToString() );
			sb.Clear();
		}
		for ( var i = 0; i < line.Length; i++ )
		{
			var c = line[i];
			if ( c == '(' || c == ')' || c == '[' || c == ']' )
			{
				Flush();
				result.Add( c.ToString() );
			}
			else if ( char.IsWhiteSpace( c ) )
			{
				Flush();
			}
			else
			{
				sb.Append( c );
			}
		}
		Flush();
		return result;
	}

	private static Vector3 ReadVector3( List<string> tokens, ref int i )
	{
		var x = ParseFloat( tokens[i++] );
		var y = ParseFloat( tokens[i++] );
		var z = ParseFloat( tokens[i++] );
		return new Vector3( x, y, z );
	}

	private static float ParseFloat( string s ) =>
		float.Parse( s, NumberStyles.Float, CultureInfo.InvariantCulture );

	#endregion

	#region Brush -> PolygonMesh

	// Reconstructs the explicit convex polygon mesh implied by a brush's set of
	// face half-spaces. We intersect every plane triple, keep the intersection
	// points that satisfy every other plane, then for each face collect those
	// points and sort them around the outward normal.
	public static bool TryBuildPolygonMesh( Brush brush, out PolygonMesh mesh, out Vector3 center )
	{
		mesh = null;
		center = Vector3.Zero;
		if ( brush is null || brush.Faces.Count < 4 ) return false;

		var faceCount = brush.Faces.Count;
		var normals = new Vector3[faceCount];
		var dists = new float[faceCount];
		for ( var i = 0; i < faceCount; i++ )
		{
			normals[i] = brush.Faces[i].PlaneNormal;
			if ( normals[i].LengthSquared < DegenerateDenomEpsilon ) return false;
			dists[i] = brush.Faces[i].PlaneDistance;
		}

		var faceVerts = new List<Vector3>[faceCount];
		for ( var i = 0; i < faceCount; i++ ) faceVerts[i] = new List<Vector3>();

		for ( var i = 0; i < faceCount; i++ )
		{
			for ( var j = i + 1; j < faceCount; j++ )
			{
				for ( var k = j + 1; k < faceCount; k++ )
				{
					if ( !TryIntersectPlanes( normals[i], dists[i], normals[j], dists[j], normals[k], dists[k], out var p ) )
					{
						continue;
					}
					var ok = true;
					for ( var m = 0; m < faceCount; m++ )
					{
						if ( m == i || m == j || m == k ) continue;
						if ( Vector3.Dot( normals[m], p ) - dists[m] > PlaneEpsilon )
						{
							ok = false;
							break;
						}
					}
					if ( !ok ) continue;
					AddUniqueVertex( faceVerts[i], p );
					AddUniqueVertex( faceVerts[j], p );
					AddUniqueVertex( faceVerts[k], p );
				}
			}
		}

		var allPoints = new List<Vector3>();
		for ( var i = 0; i < faceCount; i++ )
		{
			if ( faceVerts[i].Count < 3 ) return false;
			allPoints.AddRange( faceVerts[i] );
		}

		var min = allPoints[0];
		var max = allPoints[0];
		for ( var i = 1; i < allPoints.Count; i++ )
		{
			min = Vector3.Min( min, allPoints[i] );
			max = Vector3.Max( max, allPoints[i] );
		}
		center = (min + max) * 0.5f;

		mesh = new PolygonMesh();
		for ( var i = 0; i < faceCount; i++ )
		{
			var verts = faceVerts[i];
			SortFaceCounterClockwise( verts, normals[i] );
			var handles = new VertexHandle[verts.Count];
			for ( var v = 0; v < verts.Count; v++ )
			{
				handles[v] = mesh.AddVertex( verts[v] - center );
			}
			var face = mesh.AddFace( handles );
			// Set placeholder UV parameters using a Quake-default 64x64 texture
			// size; Player will recompute these per face once it knows the real
			// PNG dimensions, but this keeps the import legible if texture
			// loading fails.
			ApplyFaceTextureParameters( mesh, face, brush.Faces[i], center, 64f, 64f );
		}

		// AddVertex returned a fresh handle per (face, vertex) — adjacent
		// faces share positions but not handles, so the mesh as built is
		// non-manifold at every shared corner and edge. Engine ops that
		// depend on connectivity break in subtle ways: SLICE's
		// ClipFacesByPlaneAndCap needs a closed loop of shared edges on
		// the cut plane to synthesise a cap, and with disconnected
		// face boundaries it silently emits no cap, leaving the brush
		// open and unable to split. Weld coincident vertices now so
		// subsequent operations see a proper manifold convex polyhedron.
		var allVerts = mesh.VertexHandles.ToList();
		if ( allVerts.Count >= 2 )
		{
			mesh.MergeVerticesWithinDistance( allVerts, VertexMergeEpsilon, bPreConnect: false, bAveragePositions: true, out _ );
		}

		return true;
	}

	private static bool TryIntersectPlanes(
		Vector3 n0, float d0, Vector3 n1, float d1, Vector3 n2, float d2,
		out Vector3 point )
	{
		// Cramer's rule on the 3x3 plane normal matrix; degenerate (parallel)
		// triples produce a near-zero determinant and are dropped.
		var det =
			n0.x * (n1.y * n2.z - n1.z * n2.y) -
			n0.y * (n1.x * n2.z - n1.z * n2.x) +
			n0.z * (n1.x * n2.y - n1.y * n2.x);
		if ( MathF.Abs( det ) < DegenerateDenomEpsilon )
		{
			point = Vector3.Zero;
			return false;
		}
		var invDet = 1f / det;
		var c0 = new Vector3( n1.y * n2.z - n1.z * n2.y, n1.z * n2.x - n1.x * n2.z, n1.x * n2.y - n1.y * n2.x );
		var c1 = new Vector3( n2.y * n0.z - n2.z * n0.y, n2.z * n0.x - n2.x * n0.z, n2.x * n0.y - n2.y * n0.x );
		var c2 = new Vector3( n0.y * n1.z - n0.z * n1.y, n0.z * n1.x - n0.x * n1.z, n0.x * n1.y - n0.y * n1.x );
		point = (c0 * d0 + c1 * d1 + c2 * d2) * invDet;
		return true;
	}

	private static void AddUniqueVertex( List<Vector3> verts, Vector3 v )
	{
		for ( var i = 0; i < verts.Count; i++ )
		{
			if ( (verts[i] - v).LengthSquared < VertexMergeEpsilon * VertexMergeEpsilon ) return;
		}
		verts.Add( v );
	}

	private static void SortFaceCounterClockwise( List<Vector3> verts, Vector3 outwardNormal )
	{
		var centroid = Vector3.Zero;
		for ( var i = 0; i < verts.Count; i++ ) centroid += verts[i];
		centroid /= verts.Count;

		var tangent = (verts[0] - centroid).Normal;
		var bitangent = Vector3.Cross( outwardNormal, tangent ).Normal;

		verts.Sort( ( a, b ) =>
		{
			var da = a - centroid;
			var db = b - centroid;
			var aa = MathF.Atan2( Vector3.Dot( da, bitangent ), Vector3.Dot( da, tangent ) );
			var ab = MathF.Atan2( Vector3.Dot( db, bitangent ), Vector3.Dot( db, tangent ) );
			return aa.CompareTo( ab );
		} );
	}

	// PolygonMesh.ComputeFaceTextureCoordinatesFromParameters renders each
	// face-vertex UV as
	//     uv.x = (dot(uAxis, pos) / scale.x + offset.x) / max(engineSize.x, 1)
	//     uv.y = (dot(vAxis, pos) / scale.y + offset.y) / max(engineSize.y, 1)
	// where `engineSize` is the size the engine reads off the face material
	// — `material.FirstTexture.Size`, optionally overridden by the material's
	// `WorldMappingWidth`/`WorldMappingHeight` attributes. Our face material
	// is a copy of a template (`prefab.vmat`) that ships with a stand-in
	// albedo (256×256 dev texture), and `material.Set("g_tTexture", ...)`
	// rebinds the shader sampler without replacing the material's
	// `FirstTexture`. So the engine renders every face dividing by the
	// stand-in size, not the actual texture we bound — that's the 4×
	// stretch the user reported on 64×64 textures (256/64 = 4) and the
	// asymmetric stretch on 48×128 NPOT textures.
	//
	// Compensate at the import boundary by reading whatever `engineSize` the
	// engine will actually use for this face's material and folding the
	// `engineSize / actualSize` ratio into the per-face scale/offset:
	//     scale_engine  = uScale_q * actualSize / engineSize
	//     offset_engine = uOffset_q * engineSize / actualSize
	// so the renderer's `(dot/scale + offset)/engineSize` collapses to the
	// Quake formula `(dot/uScale_q + uOffset_q)/actualSize`.
	public static void ApplyFaceTextureParameters(
		PolygonMesh mesh, FaceHandle face, Face src, Vector3 center, float texW, float texH )
	{
		_ = center;
		var uScale = src.UScale == 0f ? 1f : src.UScale;
		var vScale = src.VScale == 0f ? 1f : src.VScale;
		(texW, texH) = DefaultTextureSize( texW, texH );

		Vector3 uAxis, vAxis;
		if ( src.HasExplicitAxes )
		{
			uAxis = src.UAxis;
			vAxis = src.VAxis;
		}
		else
		{
			ComputeQuakeBaseAxes( src.PlaneNormal, out uAxis, out vAxis );
			if ( src.Rotation != 0f )
			{
				var spin = Rotation.FromAxis( src.PlaneNormal, src.Rotation );
				uAxis = spin * uAxis;
				vAxis = spin * vAxis;
			}
		}

		var engineSize = GetEngineTextureSize( mesh.GetFaceMaterial( face ), texW, texH );
		var compU = engineSize.x > 1e-6f ? texW / engineSize.x : 1f;
		var compV = engineSize.y > 1e-6f ? texH / engineSize.y : 1f;

		mesh.SetFaceTextureParameters( face,
			new Vector4( uAxis, src.UOffset / compU ),
			new Vector4( vAxis, src.VOffset / compV ),
			new Vector2( uScale * compU, vScale * compV ) );
	}

	// Mirror of Sandbox.PolygonMesh.CalculateTextureSize so we can predict
	// what the engine will divide by when it evaluates this face's UVs.
	private static Vector2 GetEngineTextureSize( Material material, float fallbackW, float fallbackH )
	{
		if ( material is null ) return new Vector2( fallbackW, fallbackH );
		var tex = material.FirstTexture;
		if ( tex is null ) return new Vector2( fallbackW, fallbackH );
		return new Vector2( tex.Width, tex.Height );
	}

	// Standard Quake base-axis lookup: pick the world axis whose direction is
	// closest to the face normal, then read off the conventional U/V pair for
	// that orientation. Each row is { dominant normal direction, U, V }.
	private static readonly (Vector3 N, Vector3 U, Vector3 V)[] _quakeBaseAxes = new[]
	{
		(new Vector3(  0,  0,  1 ), new Vector3(  1,  0,  0 ), new Vector3(  0, -1,  0 )), // floor
		(new Vector3(  0,  0, -1 ), new Vector3(  1,  0,  0 ), new Vector3(  0, -1,  0 )), // ceiling
		(new Vector3(  1,  0,  0 ), new Vector3(  0,  1,  0 ), new Vector3(  0,  0, -1 )), // east wall
		(new Vector3( -1,  0,  0 ), new Vector3(  0,  1,  0 ), new Vector3(  0,  0, -1 )), // west wall
		(new Vector3(  0,  1,  0 ), new Vector3(  1,  0,  0 ), new Vector3(  0,  0, -1 )), // north wall
		(new Vector3(  0, -1,  0 ), new Vector3(  1,  0,  0 ), new Vector3(  0,  0, -1 )), // south wall
	};

	private static void ComputeQuakeBaseAxes( Vector3 normal, out Vector3 u, out Vector3 v )
	{
		var bestDot = -float.MaxValue;
		var bestIdx = 0;
		for ( var i = 0; i < _quakeBaseAxes.Length; i++ )
		{
			var dot = Vector3.Dot( normal, _quakeBaseAxes[i].N );
			if ( dot > bestDot )
			{
				bestDot = dot;
				bestIdx = i;
			}
		}
		u = _quakeBaseAxes[bestIdx].U;
		v = _quakeBaseAxes[bestIdx].V;
	}

	#endregion

	#region Export

	public static string WriteWorldspawn( IEnumerable<Brush> brushes ) =>
		WriteMap( brushes, null );

	// Full-map serializer: writes the worldspawn entity (carrying every
	// brush) followed by one entity block per additional `Entity` in
	// `entities`. Properties are emitted as `"key" "value"` lines and each
	// nested brush is written via AppendBrush. `classname` is always emitted
	// first regardless of where it appears in the entity's Properties bag.
	public static string WriteMap( IEnumerable<Brush> worldBrushes, IEnumerable<Entity> entities )
	{
		var sb = new StringBuilder();
		sb.AppendLine( "// Game: Generic" );
		sb.AppendLine( "// Format: Valve" );

		var entityIndex = 0;
		sb.Append( "// entity " ).Append( entityIndex++.ToString( CultureInfo.InvariantCulture ) ).AppendLine();
		sb.AppendLine( "{" );
		sb.AppendLine( "\"classname\" \"worldspawn\"" );
		if ( worldBrushes is not null )
		{
			var brushIdx = 0;
			foreach ( var brush in worldBrushes )
			{
				if ( brush is null ) continue;
				AppendBrush( sb, brush, brushIdx++ );
			}
		}
		sb.AppendLine( "}" );

		if ( entities is null ) return sb.ToString();
		foreach ( var entity in entities )
		{
			if ( entity is null ) continue;
			sb.Append( "// entity " ).Append( entityIndex++.ToString( CultureInfo.InvariantCulture ) ).AppendLine();
			sb.AppendLine( "{" );
			AppendKeyValue( sb, "classname",
				string.IsNullOrEmpty( entity.ClassName ) ? "info_null" : entity.ClassName );
			foreach ( var kv in entity.Properties )
			{
				// `classname` was already written above; skip duplicates that
				// happen to be in the bag (the parser stores classname in both
				// `Properties` and `ClassName`).
				if ( string.Equals( kv.Key, "classname", StringComparison.OrdinalIgnoreCase ) ) continue;
				AppendKeyValue( sb, kv.Key, kv.Value );
			}
			var brushIdx = 0;
			foreach ( var brush in entity.Brushes )
			{
				if ( brush is null ) continue;
				AppendBrush( sb, brush, brushIdx++ );
			}
			sb.AppendLine( "}" );
		}
		return sb.ToString();
	}

	private static void AppendBrush( StringBuilder sb, Brush brush, int brushIndex )
	{
		sb.Append( "// brush " ).Append( brushIndex.ToString( CultureInfo.InvariantCulture ) ).AppendLine();
		sb.AppendLine( "{" );
		foreach ( var face in brush.Faces )
		{
			AppendFace( sb, face );
		}
		sb.AppendLine( "}" );
	}

	// Escape quotes and backslashes inside MAP values so a key/value line
	// always tokenises back into exactly two strings on re-import.
	private static void AppendKeyValue( StringBuilder sb, string key, string value )
	{
		sb.Append( '"' ).Append( EscapeValue( key ?? "" ) ).Append( "\" \"" )
			.Append( EscapeValue( value ?? "" ) ).Append( '"' ).AppendLine();
	}

	private static string EscapeValue( string s )
	{
		if ( string.IsNullOrEmpty( s ) ) return s;
		if ( s.IndexOf( '"' ) < 0 && s.IndexOf( '\\' ) < 0 ) return s;
		var sb = new StringBuilder( s.Length + 4 );
		foreach ( var c in s )
		{
			if ( c == '"' || c == '\\' ) sb.Append( '\\' );
			sb.Append( c );
		}
		return sb.ToString();
	}

	private static void AppendFace( StringBuilder sb, Face face )
	{
		var c = CultureInfo.InvariantCulture;
		string F( float v ) => v.ToString( "0.######", c );
		sb.Append( "( " ).Append( F( face.P0.x ) ).Append( ' ' ).Append( F( face.P0.y ) ).Append( ' ' ).Append( F( face.P0.z ) ).Append( " ) " );
		sb.Append( "( " ).Append( F( face.P1.x ) ).Append( ' ' ).Append( F( face.P1.y ) ).Append( ' ' ).Append( F( face.P1.z ) ).Append( " ) " );
		sb.Append( "( " ).Append( F( face.P2.x ) ).Append( ' ' ).Append( F( face.P2.y ) ).Append( ' ' ).Append( F( face.P2.z ) ).Append( " ) " );
		sb.Append( string.IsNullOrEmpty( face.Texture ) ? EmptyTextureName : StripTextureExtension( face.Texture ) ).Append( ' ' );
		sb.Append( "[ " ).Append( F( face.UAxis.x ) ).Append( ' ' ).Append( F( face.UAxis.y ) ).Append( ' ' ).Append( F( face.UAxis.z ) ).Append( ' ' ).Append( F( face.UOffset ) ).Append( " ] " );
		sb.Append( "[ " ).Append( F( face.VAxis.x ) ).Append( ' ' ).Append( F( face.VAxis.y ) ).Append( ' ' ).Append( F( face.VAxis.z ) ).Append( ' ' ).Append( F( face.VOffset ) ).Append( " ] " );
		sb.Append( F( face.Rotation ) ).Append( ' ' ).Append( F( face.UScale ) ).Append( ' ' ).Append( F( face.VScale ) );
		sb.AppendLine();
	}

	// Quake/Valve MAP texture names are stems, not file paths — strip the
	// trailing image extension (`.png` / `.tga` / `.jpg` / `.bmp`, etc.)
	// from whatever the material round-tripped through `_pathFromMaterial`
	// so a re-import doesn't try to resolve `name.png.png`. Keep the
	// directory portion intact since the import loader already handles
	// path-relative names.
	private static string StripTextureExtension( string texture )
	{
		if ( string.IsNullOrEmpty( texture ) ) return texture;
		var lastDot = texture.LastIndexOf( '.' );
		if ( lastDot < 0 ) return texture;
		var lastSlash = texture.LastIndexOfAny( new[] { '/', '\\' } );
		if ( lastDot < lastSlash ) return texture;
		return texture.Substring( 0, lastDot );
	}

	private static void GetTextureSizeForMaterial( string path, out float texW, out float texH )
	{
		float w = 0f, h = 0f;
		if ( !string.IsNullOrEmpty( path ) && GameNetwork.TryGetTexture( path, out var tex ) && tex is not null )
		{
			w = tex.Width;
			h = tex.Height;
		}
		(texW, texH) = DefaultTextureSize( w, h );
	}

	// Build a MAP Brush description from an editor ModelBrush. For each polygon
	// face we emit three points listed clockwise as viewed from outside (MAP
	// convention), and capture the U/V axes back out of the face's texture
	// parameters.
	public static Brush BuildBrushFromModel( ModelBrush model )
	{
		if ( model is null || model.PolygonMesh is null ) return null;
		var poly = model.PolygonMesh;
		var transform = model.WorldTransform;
		var result = new Brush();

		// Cylinder and sphere caps are built as a triangle fan that shares
		// a single supporting plane. Quake's brush format describes a
		// brush as the intersection of half-spaces, so writing every
		// coplanar triangle out as its own face emits N copies of the
		// same half-space. The importer then reconstructs N identical
		// polygons (each cap edge ends up shared by 1 side face + N
		// duplicated caps = N+1 face uses), the manifold-edge check in
		// IsConvexClosedMesh rejects the result, and the loaded brush
		// shows up as invalid. Dedupe coplanar faces here — one face
		// per unique plane is sufficient for the importer to rebuild
		// the polygon from plane intersections.
		var seenPlanes = new List<(Vector3 normal, float dist)>();

		foreach ( var face in poly.FaceHandles )
		{
			var verts = poly.GetFaceVertices( face );
			if ( verts.Length < 3 ) continue;
			var worldVerts = new Vector3[verts.Length];
			for ( var i = 0; i < verts.Length; i++ )
			{
				worldVerts[i] = transform.PointToWorld( poly.GetVertexPosition( verts[i] ) );
			}

			// Polygon winding in this codebase is CCW from outside, but MAP
			// expects CW. Reverse the picks to flip the implied normal.
			var p0 = worldVerts[0];
			var p1 = worldVerts[worldVerts.Length - 1];
			var p2 = worldVerts[worldVerts.Length - 2];

			// Skip faces whose 3 representative points are colinear (would
			// emit a degenerate plane) and faces whose plane was already
			// emitted by an earlier triangle on the same cap.
			var planeNormalRaw = Vector3.Cross( p2 - p0, p1 - p0 );
			var planeNormalLen = planeNormalRaw.Length;
			if ( planeNormalLen < DegenerateDenomEpsilon ) continue;
			var planeNormal = planeNormalRaw / planeNormalLen;
			var planeDist = Vector3.Dot( planeNormal, p0 );

			var duplicate = false;
			foreach ( var seen in seenPlanes )
			{
				if ( Vector3.Dot( seen.normal, planeNormal ) < 1f - CoplanarNormalEpsilon ) continue;
				if ( MathF.Abs( seen.dist - planeDist ) > PlaneEpsilon ) continue;
				duplicate = true;
				break;
			}
			if ( duplicate ) continue;
			seenPlanes.Add( (planeNormal, planeDist) );

			poly.GetFaceTextureParameters( face, out var axisU, out var axisV, out var scale );
			var material = poly.GetFaceMaterial( face );
			var texture = GameNetwork.GetPathForMaterial( material ) ?? "";

			// Undo the engine-size / actual-size compensation we baked in on
			// import (see ApplyFaceTextureParameters) so the round-trip writes
			// back the original Quake scale/offset. We don't have the actual
			// texture pixel size at this point — Quake's `uScale_q = 1` brush
			// pulls back as `scale.x / compU` where `compU = texW / engineW`.
			// In the common case the material's FirstTexture *is* the
			// stand-in (so compU = texW / 256), and editor-authored brushes
			// have `scale = 0.25 * compU` which collapses back to 1.
			GetTextureSizeForMaterial( texture, out var actualW, out var actualH );
			var engineSize = GetEngineTextureSize( material, actualW, actualH );
			var compU = engineSize.x > 1e-6f && actualW > 1e-6f ? actualW / engineSize.x : 1f;
			var compV = engineSize.y > 1e-6f && actualH > 1e-6f ? actualH / engineSize.y : 1f;

			var unitU = new Vector3( axisU.x, axisU.y, axisU.z );
			var unitV = new Vector3( axisV.x, axisV.y, axisV.z );
			var uScale = MathF.Abs( scale.x ) > 1e-6f && MathF.Abs( compU ) > 1e-6f
				? scale.x / compU : DefaultExportUVScale;
			var vScale = MathF.Abs( scale.y ) > 1e-6f && MathF.Abs( compV ) > 1e-6f
				? scale.y / compV : DefaultExportUVScale;
			var uOffset = axisU.w * compU;
			var vOffset = axisV.w * compV;

			result.Faces.Add( new Face
			{
				P0 = p0,
				P1 = p1,
				P2 = p2,
				Texture = texture,
				UAxis = unitU,
				UOffset = uOffset,
				VAxis = unitV,
				VOffset = vOffset,
				Rotation = 0f,
				UScale = uScale,
				VScale = vScale,
			} );
		}
		return result;
	}

	#endregion
}
