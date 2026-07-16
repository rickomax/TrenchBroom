using System;
using System.Collections.Generic;
using System.Linq;
using HalfEdgeMesh;
using Sandbox;
using static Sandbox.PolygonMesh;

namespace LibCSG;

// A convex polyhedron stored as a set of half-spaces. Each plane's normal points
// OUT of the brush; a point P is inside the brush iff plane.GetDistance(P) <= 0
// for every plane. The polygon stored on each plane is derived from the full
// plane set, listed CCW as seen from outside (looking down the plane normal).
//
// Concavity is represented by a List<ConvexBrush>. Individual brushes are always
// convex by construction: primitives (box, cylinder, sphere) build one brush,
// and the boolean operators emit a list whose union forms the (possibly concave)
// result. The mesh path's "fold everything into one PolygonMesh" never happens
// here.
public sealed class ConvexBrush
{
	public sealed class BrushPlane
	{
		public Plane Plane;
		public List<Vector3> Polygon = new();
		public Material Material;
		public Vector4 AxisU;
		public Vector4 AxisV;
		public Vector2 Scale = new( 0.25f, 0.25f );
	}

	public List<BrushPlane> Planes { get; } = new();

	private const float PointOnPlaneEpsilon = 1e-3f;
	private const float CoplanarNormalEpsilon = 0.9995f;
	private const float ParallelTripleEpsilon = 1e-6f;
	private const float MinPolygonArea = 1e-6f;
	// Used by AddVertexDedup during per-plane polygon construction. Kept
	// tight: candidates here come from triple-plane intersections on the
	// same brush, where genuinely-distinct corners can sit close together
	// after chained CSG, and over-merging here would drop a real corner.
	private const float VertexWeldEpsilon = 1e-4f;
	// Looser epsilon for ToPolygonMesh's final cross-plane weld. Adjacent
	// planes' polygons each carry their own copy of a shared corner, each
	// computed via a different triple (Cramer's rule); their error
	// commonly exceeds 1e-4 for ill-conditioned plane triples produced by
	// chained subtractions, and unmerged copies break the closed-manifold
	// edge check downstream (IsConvexClosedMesh flags the brush invalid).
	// 0.01 matches the boolean / map-import weld epsilon used elsewhere.
	private const float OutputWeldEpsilon = 0.01f;

	public ConvexBrush Clone()
	{
		var dst = new ConvexBrush();
		foreach ( var p in Planes )
		{
			dst.Planes.Add( new BrushPlane
			{
				Plane = p.Plane,
				Polygon = new List<Vector3>( p.Polygon ),
				Material = p.Material,
				AxisU = p.AxisU,
				AxisV = p.AxisV,
				Scale = p.Scale,
			} );
		}
		return dst;
	}

	// Build a single brush from a PolygonMesh whose faces all lie on the outer
	// surface of one convex hull. Caller guarantees convexity — true for fresh
	// primitives. A later op's result will be a List<ConvexBrush>; reading one
	// of those back requires the caller to track the list directly.
	public static ConvexBrush FromConvexPolygonMesh( PolygonMesh mesh )
	{
		var brush = new ConvexBrush();
		foreach ( var face in mesh.FaceHandles )
		{
			var verts = mesh.GetFaceVertices( face );
			if ( verts.Length < 3 ) continue;

			var positions = new Vector3[verts.Length];
			for ( int i = 0; i < verts.Length; i++ )
				positions[i] = mesh.GetVertexPosition( verts[i] );

			var normal = NewellNormal( positions );
			if ( normal.LengthSquared < 1e-12f ) continue;
			normal = normal.Normal;

			mesh.GetFaceTextureParameters( face, out var axisU, out var axisV, out var scale );

			brush.Planes.Add( new BrushPlane
			{
				Plane = new Plane( positions[0], normal ),
				Polygon = positions.ToList(),
				Material = mesh.GetFaceMaterial( face ),
				AxisU = axisU,
				AxisV = axisV,
				Scale = scale,
			} );
		}
		brush.RebuildPolygons();
		return brush;
	}

	// Recompute each plane's polygon by intersecting it with every other plane's
	// half-space. Planes whose polygon ends up empty (redundant) are dropped.
	//
	// Each unique plane triple's intersection is computed ONCE here. The
	// resulting point is then added to every plane it touches (i, j, k), so a
	// corner shared between adjacent face polygons gets bit-for-bit identical
	// coordinates across all of them — no per-triple drift from independent
	// Cramer's-rule recomputation. Without this, the same physical corner used
	// to be Cramer-recomputed once per face it belongs to, and the three
	// independent rounding errors blew past the cross-plane weld epsilon for
	// ill-conditioned triples (most visibly: sphere ∩ box at the equator).
	public void RebuildPolygons()
	{
		var n = Planes.Count;
		var buckets = new List<Vector3>[n];
		for ( int i = 0; i < n; i++ ) buckets[i] = new List<Vector3>();

		for ( int i = 0; i < n; i++ )
		{
			for ( int j = i + 1; j < n; j++ )
			{
				for ( int k = j + 1; k < n; k++ )
				{
					if ( !TripleIntersect( Planes[i].Plane, Planes[j].Plane, Planes[k].Plane, out var pt ) )
						continue;
					if ( !PointInsideBrush( pt ) ) continue;
					AddVertexDedup( buckets[i], pt );
					AddVertexDedup( buckets[j], pt );
					AddVertexDedup( buckets[k], pt );
				}
			}
		}

		var keep = new List<BrushPlane>( n );
		for ( int i = 0; i < n; i++ )
		{
			var planeI = Planes[i];
			var verts = buckets[i];
			if ( verts.Count < 3 ) continue;
			SortAroundNormal( verts, planeI.Plane.Normal );
			if ( PolygonArea( verts, planeI.Plane.Normal ) < MinPolygonArea ) continue;
			planeI.Polygon = verts;
			keep.Add( planeI );
		}
		Planes.Clear();
		Planes.AddRange( keep );
	}

	// Split this brush by a cutting plane. Front-half is on the side the cutting
	// plane's normal points to ("outside" of the brush the cutter came from);
	// back-half is on the opposite side. The cap face inherits the cutter's
	// material/UV axes. Either half is null if the brush is entirely on one side.
	public void ClipByPlane( BrushPlane cutter, out ConvexBrush front, out ConvexBrush back )
	{
		front = null;
		back = null;

		bool anyFront = false, anyBack = false;
		foreach ( var bp in Planes )
		{
			foreach ( var v in bp.Polygon )
			{
				var d = cutter.Plane.GetDistance( v );
				if ( d > PointOnPlaneEpsilon ) anyFront = true;
				else if ( d < -PointOnPlaneEpsilon ) anyBack = true;
				if ( anyFront && anyBack ) goto split;
			}
		}
		if ( !anyFront ) { back = Clone(); return; }
		if ( !anyBack ) { front = Clone(); return; }

split:
		// back = original ∩ (cutter half-space where d <= 0). Plane normal as-is.
		var backCandidate = Clone();
		backCandidate.Planes.Add( CapPlane( cutter, flip: false ) );
		backCandidate.RebuildPolygons();
		if ( backCandidate.Planes.Count >= 4 ) back = backCandidate;

		// front = original ∩ (cutter half-space where d >= 0). Plane normal flipped.
		var frontCandidate = Clone();
		frontCandidate.Planes.Add( CapPlane( cutter, flip: true ) );
		frontCandidate.RebuildPolygons();
		if ( frontCandidate.Planes.Count >= 4 ) front = frontCandidate;
	}

	public static List<ConvexBrush> PerformBoolean( IEnumerable<ConvexBrush> a, IEnumerable<ConvexBrush> b, BooleanOperation op )
	{
		var listA = a.Select( x => x.Clone() ).ToList();
		var listB = b.Select( x => x.Clone() ).ToList();
		return op switch
		{
			BooleanOperation.Subtract => Subtract( listA, listB ),
			BooleanOperation.Union => Union( listA, listB ),
			BooleanOperation.Intersect => Intersect( listA, listB ),
			_ => listA,
		};
	}

	public static List<ConvexBrush> Subtract( List<ConvexBrush> a, List<ConvexBrush> b )
	{
		var result = new List<ConvexBrush>( a );
		foreach ( var bBrush in b )
		{
			var next = new List<ConvexBrush>( result.Count );
			foreach ( var aBrush in result )
				next.AddRange( SubtractConvex( aBrush, bBrush ) );
			result = next;
		}
		return result;
	}

	public static List<ConvexBrush> Union( List<ConvexBrush> a, List<ConvexBrush> b )
	{
		// Subtract(A, B) emits the part of A outside B; appending B fills it in.
		// Output may have neighbouring brushes that share coplanar faces; that's
		// expected for a brush-list representation and is what .map writers want.
		var result = Subtract( a, b );
		foreach ( var bBrush in b )
			result.Add( bBrush.Clone() );
		return result;
	}

	public static List<ConvexBrush> Intersect( List<ConvexBrush> a, List<ConvexBrush> b )
	{
		var result = new List<ConvexBrush>();
		foreach ( var aBrush in a )
		{
			foreach ( var bBrush in b )
			{
				var inter = IntersectConvex( aBrush, bBrush );
				if ( inter != null ) result.Add( inter );
			}
		}
		return result;
	}

	// Carve one convex brush by another. Walks b's planes in order; for plane Pi,
	// emit the fragment of `remaining` on the FRONT of Pi (outside b w.r.t. Pi),
	// then keep carving the BACK. After all planes processed the back is entirely
	// inside b and is discarded.
	private static IEnumerable<ConvexBrush> SubtractConvex( ConvexBrush a, ConvexBrush b )
	{
		if ( !AABBOverlap( a, b ) ) return new[] { a.Clone() };

		var fragments = new List<ConvexBrush>();
		var remaining = a.Clone();
		foreach ( var bp in b.Planes )
		{
			remaining.ClipByPlane( bp, out var front, out var back );
			if ( front != null ) fragments.Add( front );
			if ( back == null ) return fragments;
			remaining = back;
		}
		// remaining is entirely inside b — discard.
		return fragments;
	}

	private static ConvexBrush IntersectConvex( ConvexBrush a, ConvexBrush b )
	{
		if ( !AABBOverlap( a, b ) ) return null;

		var brush = new ConvexBrush();
		foreach ( var p in a.Planes )
		{
			brush.Planes.Add( new BrushPlane
			{
				Plane = p.Plane,
				Material = p.Material,
				AxisU = p.AxisU,
				AxisV = p.AxisV,
				Scale = p.Scale,
			} );
		}
		foreach ( var p in b.Planes )
		{
			brush.Planes.Add( new BrushPlane
			{
				Plane = p.Plane,
				Material = p.Material,
				AxisU = p.AxisU,
				AxisV = p.AxisV,
				Scale = p.Scale,
			} );
		}
		brush.RebuildPolygons();
		return brush.Planes.Count >= 4 ? brush : null;
	}

	// Build a PolygonMesh from a brush list. Each plane becomes one face; verts
	// are welded across brushes by MergeVerticesWithinDistance. Internal coplanar
	// pairs (cavity walls shared between adjacent fragments) are NOT removed —
	// backface culling hides them at render time but they're still in the model.
	// TODO: pre-merge step that detects opposite-normal coplanar polygon pairs
	// between brushes and drops both before AddFace.
	public static PolygonMesh ToPolygonMesh( IEnumerable<ConvexBrush> brushes )
	{
		var mesh = new PolygonMesh();
		foreach ( var brush in brushes )
		{
			foreach ( var bp in brush.Planes )
			{
				if ( bp.Polygon.Count < 3 ) continue;
				var handles = new VertexHandle[bp.Polygon.Count];
				for ( int i = 0; i < bp.Polygon.Count; i++ )
					handles[i] = mesh.AddVertex( bp.Polygon[i] );

				var face = mesh.AddFace( handles );
				if ( !face.IsValid ) continue;

				if ( bp.Material != null ) mesh.SetFaceMaterial( face, bp.Material );
				mesh.SetFaceTextureParameters( face, bp.AxisU, bp.AxisV, bp.Scale );
			}
		}

		var verts = mesh.VertexHandles.ToList();
		if ( verts.Count >= 2 )
			mesh.MergeVerticesWithinDistance( verts, OutputWeldEpsilon, bPreConnect: false, bAveragePositions: true, out _ );

		return mesh;
	}

	// --- helpers ---

	private static BrushPlane CapPlane( BrushPlane src, bool flip )
	{
		if ( !flip )
		{
			return new BrushPlane
			{
				Plane = src.Plane,
				Material = src.Material,
				AxisU = src.AxisU,
				AxisV = src.AxisV,
				Scale = src.Scale,
			};
		}
		// Reconstruct plane with reversed normal. Origin-closest point of the
		// original plane is N * d (with N unit-length and d = -GetDistance(0)).
		var d = -src.Plane.GetDistance( Vector3.Zero );
		var pointOnPlane = src.Plane.Normal * d;
		return new BrushPlane
		{
			Plane = new Plane( pointOnPlane, -src.Plane.Normal ),
			Material = src.Material,
			AxisU = src.AxisU,
			AxisV = src.AxisV,
			Scale = src.Scale,
		};
	}

	private bool PointSatisfiesAllPlanes( Vector3 pt, int skipIdx )
	{
		for ( int i = 0; i < Planes.Count; i++ )
		{
			if ( i == skipIdx ) continue;
			if ( Planes[i].Plane.GetDistance( pt ) > PointOnPlaneEpsilon )
				return false;
		}
		return true;
	}

	// "Is pt inside (or on) the brush?" — every plane's signed distance has
	// to be ≤ PointOnPlaneEpsilon. RebuildPolygons uses this on triple-plane
	// intersections, where the three source planes contribute distance ≈ 0
	// automatically and the check is really about every OTHER plane.
	private bool PointInsideBrush( Vector3 pt )
	{
		for ( int i = 0; i < Planes.Count; i++ )
		{
			if ( Planes[i].Plane.GetDistance( pt ) > PointOnPlaneEpsilon )
				return false;
		}
		return true;
	}

	private static bool TripleIntersect( Plane a, Plane b, Plane c, out Vector3 pt )
	{
		// Cramer's rule for n_i . p = d_i, with d_i = -GetDistance(origin).
		var n1 = a.Normal;
		var n2 = b.Normal;
		var n3 = c.Normal;
		var det = Vector3.Dot( n1, Vector3.Cross( n2, n3 ) );
		if ( MathF.Abs( det ) < ParallelTripleEpsilon )
		{
			pt = default;
			return false;
		}
		var d1 = -a.GetDistance( Vector3.Zero );
		var d2 = -b.GetDistance( Vector3.Zero );
		var d3 = -c.GetDistance( Vector3.Zero );
		pt = (d1 * Vector3.Cross( n2, n3 ) + d2 * Vector3.Cross( n3, n1 ) + d3 * Vector3.Cross( n1, n2 )) / det;
		return true;
	}

	private static void AddVertexDedup( List<Vector3> verts, Vector3 v )
	{
		foreach ( var existing in verts )
		{
			if ( (existing - v).LengthSquared < VertexWeldEpsilon * VertexWeldEpsilon )
				return;
		}
		verts.Add( v );
	}

	private static void SortAroundNormal( List<Vector3> verts, Vector3 normal )
	{
		if ( verts.Count <= 2 ) return;
		var centroid = Vector3.Zero;
		foreach ( var v in verts ) centroid += v;
		centroid /= verts.Count;

		// Pick an in-plane basis that's not degenerate with `normal`.
		var seed = MathF.Abs( normal.z ) < 0.9f ? new Vector3( 0, 0, 1 ) : new Vector3( 1, 0, 0 );
		var u = Vector3.Cross( normal, seed ).Normal;
		var w = Vector3.Cross( normal, u ).Normal;

		verts.Sort( ( a, b ) =>
		{
			var da = a - centroid;
			var db = b - centroid;
			var aa = MathF.Atan2( Vector3.Dot( da, w ), Vector3.Dot( da, u ) );
			var ab = MathF.Atan2( Vector3.Dot( db, w ), Vector3.Dot( db, u ) );
			return aa.CompareTo( ab );
		} );
	}

	private static float PolygonArea( IReadOnlyList<Vector3> verts, Vector3 normal )
	{
		if ( verts.Count < 3 ) return 0f;
		var sum = Vector3.Zero;
		for ( int i = 0; i < verts.Count; i++ )
		{
			var a = verts[i];
			var b = verts[(i + 1) % verts.Count];
			sum += Vector3.Cross( a, b );
		}
		return MathF.Abs( Vector3.Dot( sum, normal ) ) * 0.5f;
	}

	private static Vector3 NewellNormal( IReadOnlyList<Vector3> verts )
	{
		var n = Vector3.Zero;
		for ( int i = 0; i < verts.Count; i++ )
		{
			var a = verts[i];
			var b = verts[(i + 1) % verts.Count];
			n.x += (a.y - b.y) * (a.z + b.z);
			n.y += (a.z - b.z) * (a.x + b.x);
			n.z += (a.x - b.x) * (a.y + b.y);
		}
		return n;
	}

	private static bool AABBOverlap( ConvexBrush a, ConvexBrush b )
	{
		AABB( a, out var aMin, out var aMax );
		AABB( b, out var bMin, out var bMax );
		return aMin.x <= bMax.x && aMax.x >= bMin.x
			&& aMin.y <= bMax.y && aMax.y >= bMin.y
			&& aMin.z <= bMax.z && aMax.z >= bMin.z;
	}

	private static void AABB( ConvexBrush brush, out Vector3 min, out Vector3 max )
	{
		min = new Vector3( float.MaxValue );
		max = new Vector3( float.MinValue );
		foreach ( var bp in brush.Planes )
		{
			foreach ( var v in bp.Polygon )
			{
				min = Vector3.Min( min, v );
				max = Vector3.Max( max, v );
			}
		}
	}
}
