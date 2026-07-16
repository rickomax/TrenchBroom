using HalfEdgeMesh;
using LibCSG;
using System;
using System.Linq;

namespace Sandbox;

/// <summary>
/// Bridges <see cref="PolygonMesh"/> with the <see cref="LibCSG"/> CSG kernel.
///
/// <para>Replaces the engine's <c>PerformBoolean</c> for ModelBrush operations: builds
/// a <see cref="CSGBrush"/> from each PolygonMesh, runs <see cref="CSGBrushOperation"/>,
/// then writes the result back into the destination PolygonMesh while preserving each
/// face's material and texture parameters.</para>
///
/// <para>The merge runs in <b>A's local space</b>. LibCSG's tolerances are tuned for
/// unit-cube-ish coordinates (<c>vertex_snap = 0.001</c>, <c>distance_tolerance = 0.3</c>,
/// <c>vertex_tolerance = 1e-10</c>); running the merge in world space at arbitrary
/// scale breaks those assumptions and causes <see cref="Build2DFaces"/> to spin on
/// near-degenerate intersections. To still handle non-uniform scale + rotation
/// correctly, B's vertices are brought into A's local space <i>per vertex</i> via
/// <c>a.Transform.PointToLocal(b.Transform.PointToWorld(v))</c>. The single
/// <c>relativeTransform</c> approach that the engine's <c>PerformBoolean</c> uses
/// is lossy in that case — composing it via <c>ToLocal</c> drops the shear that arises
/// when scale is non-uniform and rotation isn't axis-aligned with that scale, showing
/// up as an axis appearing "halved". Doing the round-trip per-vertex sidesteps the
/// compression into a single Transform entirely.</para>
///
/// <para>Source-face provenance is tracked through LibCSG via the <c>source_face_index</c>
/// field added to <see cref="MeshMerge.Face"/> and <see cref="CSGBrush.Face"/>. After the
/// boolean we look up the originating PolygonMesh face by index, copy its material onto
/// the new face, and let the engine re-derive texture axes from the interpolated UVs that
/// LibCSG already produced.</para>
/// </summary>
public static class PolygonMeshCSGBridge
{
	private readonly record struct VertexKey( int X, int Y, int Z )
	{
		public static VertexKey From( Vector3 v, float snap ) =>
			new(
				(int)MathF.Round( v.x / snap ),
				(int)MathF.Round( v.y / snap ),
				(int)MathF.Round( v.z / snap )
			);
	}
	/// <summary>
	/// Build a triangulated <see cref="CSGBrush"/> from a <see cref="PolygonMesh"/>,
	/// applying <paramref name="transformPoint"/> to every vertex on the way in. Pass
	/// <c>v =&gt; v</c> when the mesh is already in the desired space; pass a per-vertex
	/// round-trip function (see <see cref="PerformBooleanCSG"/>) to bring another mesh
	/// into A's local space without going through a lossy single-Transform composition.
	/// </summary>
	/// <param name="sourceFaceHandles">
	/// Parallel to <c>brush.faces</c>: entry <c>i</c> is the originating
	/// <see cref="FaceHandle"/> for fan-triangle <c>i</c>. Used to look up materials
	/// after the merge.
	/// </param>
	public static CSGBrush ToCSGBrush( PolygonMesh mesh, Func<Vector3, Vector3> transformPoint, out List<FaceHandle> sourceFaceHandles )
	{
		var brush = new CSGBrush();
		var verts = new List<Vector3>();
		var uvs = new List<Vector2>();
		sourceFaceHandles = new List<FaceHandle>();

		foreach ( var face in mesh.FaceHandles )
		{
			if ( !face.IsValid )
				continue;

			if ( !mesh.GetVerticesConnectedToFace( face, out var faceVerts ) || faceVerts == null || faceVerts.Length < 3 )
				continue;

			var faceUVs = mesh.GetFaceTextureCoords( face );

			// Fan-triangulate: (v0, vi, vi+1). UVs are per-half-edge, in the same
			// order as the vertex list returned by GetVerticesConnectedToFace.
			var p0 = transformPoint( mesh.GetVertexPosition( faceVerts[0] ) );
			var u0 = faceUVs.Length > 0 ? faceUVs[0] : Vector2.Zero;

			for ( int i = 1; i < faceVerts.Length - 1; i++ )
			{
				var p1 = transformPoint( mesh.GetVertexPosition( faceVerts[i] ) );
				var p2 = transformPoint( mesh.GetVertexPosition( faceVerts[i + 1] ) );
				var u1 = faceUVs.Length > i ? faceUVs[i] : Vector2.Zero;
				var u2 = faceUVs.Length > (i + 1) ? faceUVs[i + 1] : Vector2.Zero;

				verts.Add( p0 ); verts.Add( p1 ); verts.Add( p2 );
				uvs.Add( u0 ); uvs.Add( u1 ); uvs.Add( u2 );
				sourceFaceHandles.Add( face );
			}
		}

		brush.build_from_faces( verts, uvs );
		return brush;
	}

	/// <summary>
	/// Drop-in replacement for <c>PolygonMesh.PerformBoolean</c>: same parameter list
	/// and same return semantics. Uses LibCSG's triangle-level CSG kernel (with proper
	/// coplanar-face handling via <see cref="Build2DFaces"/>) instead of the engine's
	/// plane-clip / point-in-mesh pipeline. Per-face materials and texture parameters
	/// from both source meshes are preserved on the result.
	///
	/// <para>The <paramref name="relativeTransform"/> parameter is accepted for signature
	/// parity with <c>PolygonMesh.PerformBoolean</c> but is <b>not used</b>. The
	/// conversion from <paramref name="other"/>'s local space to <c>this</c>'s local
	/// space is computed per-vertex from each mesh's <see cref="PolygonMesh.Transform"/>
	/// (which ModelBrush keeps in sync with <c>GameObject.WorldTransform</c>):
	/// <c>v' = this.Transform.PointToLocal(other.Transform.PointToWorld(v))</c>.
	/// This is exact for any scale/rotation combination; the single-Transform composition
	/// that <c>relativeTransform</c> represents silently drops shear when either mesh has
	/// non-uniform <c>WorldScale</c> combined with a non-axis-aligned rotation, which
	/// shows up as an axis appearing "halved" in the result.</para>
	/// </summary>
	public static bool PerformBooleanCSG( this PolygonMesh a, PolygonMesh other, Transform relativeTransform, PolygonMesh.BooleanOperation op )
	{
		_ = relativeTransform; // see docstring — kept for signature parity only.

		if ( a == null || other == null )
			return false;

		// Snapshot the two meshes' Transforms once. The bToA round-trip below relies on
		// both being kept current with the owning GameObject's WorldTransform.
		var aWorld = a.Transform;
		var bWorld = other.Transform;

		// A's vertices stay in A's local space. B's vertices go through world to land in
		// A's local space — exact under any scale/rotation, unlike a single composed
		// Transform. The merge then operates entirely in A's local coordinate frame, so
		// LibCSG's tolerances (tuned for ~unit-cube magnitudes) keep working.
		var brushA = ToCSGBrush( a, v => v, out var sourceFacesA );
		var brushB = ToCSGBrush( other, v => aWorld.PointToLocal( bWorld.PointToWorld( v ) ), out var sourceFacesB );

		var materialsA = sourceFacesA.Select( a.GetFaceMaterial ).ToList();
		var materialsB = sourceFacesB.Select( other.GetFaceMaterial ).ToList();

		var libOp = op switch
		{
			PolygonMesh.BooleanOperation.Union => Operation.OPERATION_UNION,
			PolygonMesh.BooleanOperation.Intersect => Operation.OPERATION_INTERSECTION,
			PolygonMesh.BooleanOperation.Subtract => Operation.OPERATION_SUBTRACTION,
			_ => Operation.OPERATION_UNION,
		};

		var merged = new CSGBrush();
		var operation = new CSGBrushOperation();
		try
		{
			operation.merge_brushes( libOp, brushA, brushB, ref merged, 0.001f );

			if ( merged.faces == null || merged.faces.Length == 0 )
			{
				// Empty result — clear A and report success (e.g. INTERSECT with no overlap).
				a.RemoveFaces( a.FaceHandles.ToList() );
				return true;
			}

			WriteMergedToPolygonMesh( a, merged, materialsA, materialsB );
			return true;
		}
		finally
		{
			// CSGBrush() allocates a scene-tracked GameObject for its transform; clean
			// up so repeated booleans don't leak nodes into the scene graph.
			brushA.obj?.Destroy();
			brushB.obj?.Destroy();
			merged.obj?.Destroy();
		}
	}

	/// <summary>
	/// Replace the contents of <paramref name="dst"/> with the merged CSG brush. The
	/// merged brush's vertices are already in <paramref name="dst"/>'s local space
	/// (see <see cref="PerformBooleanCSG"/>), so they're written as-is. Welds shared
	/// vertices, applies per-face material from the snapshot lists, then asks the
	/// engine to derive texture parameters from the interpolated UVs LibCSG produced.
	/// Coplanar adjacent faces with the same material are merged back into n-gons so
	/// the output looks like a brush, not a triangle soup.
	/// </summary>
	private static void WriteMergedToPolygonMesh( PolygonMesh dst, CSGBrush merged, IReadOnlyList<Material> materialsA, IReadOnlyList<Material> materialsB )
	{
		dst.RemoveFaces( dst.FaceHandles.ToList() );

		// Vertex dedupe by quantized position instead of exact float equality.
		// Exact keys can leave near-duplicate verts unmerged, which later shows up
		// as tiny cracks/invalid holes after face assembly.
		const float weldSnap = 0.001f;
		var vertexCache = new Dictionary<VertexKey, VertexHandle>( merged.faces.Length * 3 );
		var newFaces = new List<FaceHandle>( merged.faces.Length );
		var newFaceUVs = new List<Vector2[]>( merged.faces.Length );
		var newFaceMaterials = new List<Material>( merged.faces.Length );

		foreach ( var face in merged.faces )
		{
			if ( face.vertices == null || face.vertices.Count < 3 )
				continue;

			// CSGBrush.build_from_faces reverses input winding on the way in, and
			// CSGBrush.getMesh reverses it again on the way out. We bypass getMesh
			// (we read merged.faces directly), so we have to do the un-reversal
			// here — otherwise every output face comes back flipped. Reverse UVs
			// in lockstep so they stay paired with their vertex.
			int n = face.vertices.Count;
			var handles = new VertexHandle[n];
			var reversedUVs = (face.uvs != null && face.uvs.Length >= n) ? new Vector2[n] : null;
			bool degenerate = false;

			for ( int i = 0; i < n; i++ )
			{
				int src = n - 1 - i;
				var p = face.vertices[src];
				var key = VertexKey.From( p, weldSnap );
				if ( !vertexCache.TryGetValue( key, out var h ) )
				{
					h = dst.AddVertex( p );
					vertexCache[key] = h;
				}
				handles[i] = h;
				if ( reversedUVs != null )
					reversedUVs[i] = face.uvs[src];
			}

			// Skip triangles whose vertices snapped together.
			for ( int i = 0; i < handles.Length && !degenerate; i++ )
			{
				for ( int j = i + 1; j < handles.Length && !degenerate; j++ )
				{
					if ( handles[i].Index == handles[j].Index )
						degenerate = true;
				}
			}
			if ( degenerate )
				continue;

			var fh = dst.AddFace( handles );
			if ( !fh.IsValid )
				continue;

			Material mat = null;
			if ( face.from_b )
			{
				if ( face.source_face_index >= 0 && face.source_face_index < materialsB.Count )
					mat = materialsB[face.source_face_index];
			}
			else
			{
				if ( face.source_face_index >= 0 && face.source_face_index < materialsA.Count )
					mat = materialsA[face.source_face_index];
			}

			if ( mat != null )
				dst.SetFaceMaterial( fh, mat );

			newFaces.Add( fh );
			newFaceUVs.Add( reversedUVs );
			newFaceMaterials.Add( mat );
		}

		// Apply the per-vertex UVs LibCSG produced (interpolated from source faces)
		// before deriving texture parameters from them.
		for ( int i = 0; i < newFaces.Count; i++ )
		{
			if ( newFaces[i].IsValid && newFaceUVs[i] != null && newFaceUVs[i].Length >= 3 )
				dst.SetFaceTextureCoords( newFaces[i], newFaceUVs[i] );
		}

		// Re-derive (axisU, axisV, scale, offset) from the UVs we just wrote. The
		// engine's renderer will subsequently re-derive UVs from these parameters,
		// matching whatever projection the source face used.
		dst.ComputeFaceTextureParametersFromCoordinates( newFaces );

		// Keep triangle output as-is. Combining faces here has been observed to create
		// invalid polygon loops and UV distortion in some coplanar merge cases.
	}
}
