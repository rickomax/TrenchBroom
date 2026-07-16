using HalfEdgeMesh;
using System;
using System.Collections.Generic;

namespace Sandbox;

// Local replacement for the engine's internal Sandbox.CustomOverlay. Only the
// methods this project actually uses are ported (Line, two Box overloads, and
// a Face overload that draws a filled, translucent face fill on top of the
// usual line outline).
//
// Unlike Gizmo.Draw — whose LineMaterial is a private static field hard-loaded
// from materials/gizmo/line.vmat — this version exposes LineMaterial as a
// settable property so callers can swap in their own material (different
// shader, depth behaviour, blend mode, etc). Default falls back to the gizmo
// line material so existing call sites work with zero setup.
//
// Each Line / Box call carries a per-segment line thickness. Render() batches
// consecutive segments with the same thickness into one Graphics.Draw, pushing
// the value through the "LineThickness" RenderAttribute on the line material
// so the shader can use it. Default 1f preserves the original look for
// untouched call sites.
//
// Face() pushes a PolygonMesh face's triangles into a separate buffer that's
// drawn with FaceMaterial. PolygonMesh.CreateFace already triangulates and
// transforms the verts into world space, so the buffer is ready to feed
// straight into Graphics.Draw as a triangle list. The face pass runs BEFORE
// the line pass each frame so outlines render on top of their own fill.
//
// duration / transform / overlay parameters are accepted for source-compat
// with the original signatures but not honoured: every draw is single-frame
// and all current call sites pass default(Transform). Cap depth/overlay
// behaviour by configuring LineMaterial.
public class CustomOverlay : Component
{
	public static CustomOverlay Instance { get; private set; }

	[Property]
	private Material _defaultLineMaterial;

	// Material used for the selection-fill pass (see Face() / Render()).
	// Should be translucent and double-sided so the overlay reads cleanly on
	// faces in any orientation; e.g. materials/tools/vertex_color_translucent.vmat.
	// Skip the face pass entirely if no material is wired up.
	[Property]
	private Material _faceMaterial;

	// _lineVerts is touched from multiple threads (call sites add from
	// wherever; Render drains from the render thread). List<T> is not
	// thread-safe, so every read/write goes through _lock. Batched adds
	// (e.g. a Box's 24 verts) take the lock once for the whole batch so
	// partial boxes can't appear in a frame.
	private readonly object _lock = new();
	private readonly List<Vertex> _lineVerts = new( 4096 );
	// One entry per LINE SEGMENT (i.e. one float per pair of consecutive
	// verts in _lineVerts). Render batches segments by this value so a frame
	// can mix multiple thicknesses without per-line state changes.
	private readonly List<float> _lineThicknesses = new( 2048 );
	// Triangle list (3 verts per triangle) for selection-fill overlays.
	// CreateFace returns world-space verts that are already triangulated, so
	// we just splice them in.
	private readonly List<Vertex> _faceVerts = new( 4096 );

	private Vertex[] _drawBuffer = new Vertex[256];
	private float[] _drawThickness = new float[128];
	private Vertex[] _groupBuffer = new Vertex[256];
	private Vertex[] _faceBuffer = new Vertex[256];

	private SceneCustomObject _renderObject;

	protected override void OnAwake()
	{
		Instance = this;
		_renderObject = new SceneCustomObject( Scene.SceneWorld )
		{
			RenderOverride = Render,
			Bounds = new BBox( new Vector3( float.MinValue ), new Vector3( float.MaxValue ) ),
		};
		base.OnAwake();
	}

	public static void Line( Vector3 a, Vector3 b, Color color, float duration = 0f, Transform transform = default, bool overlay = false, float thickness = 1f )
	{
		var c = (Color32)color;
		lock ( Instance._lock )
		{
			Instance._lineVerts.Add( new Vertex( a ) { Color = c } );
			Instance._lineVerts.Add( new Vertex( b ) { Color = c } );
			Instance._lineThicknesses.Add( thickness );
		}
	}

	public static void Box( BBox bbox, Color color, float duration = 0f, Transform transform = default, bool overlay = false, float thickness = 1f )
	{
		var c = (Color32)color;
		lock ( Instance._lock )
		{
			AddBoxEdges( bbox, c, thickness );
		}
	}

	public static void Box( Vector3 position, Vector3 size, Color color, float duration = 0f, Transform transform = default, bool overlay = false, float thickness = 1f )
	{
		var half = size * 0.5f;
		Box( new BBox( position - half, position + half ), color, duration, transform, overlay, thickness );
	}

	// Queue a PolygonMesh face for the selection-fill pass. The mesh's
	// CreateFace pulls the face's triangulated vertices (already transformed
	// into world space) and tags each with `color`. No-op if the mesh
	// doesn't recognise the face handle, or if the overlay's FaceMaterial
	// isn't set.
	public static void Face( PolygonMesh mesh, FaceHandle face, Transform transform, Color color )
	{
		if ( mesh is null ) return;
		if ( Instance._faceMaterial is null ) return;
		var verts = mesh.CreateFace( face, transform, color );
		if ( verts is null || verts.Length == 0 ) return;
		lock ( Instance._lock )
		{
			Instance._faceVerts.AddRange( verts );
		}
	}

	// SceneCustomObject's RenderOverride fires once per frame for the scene.
	// Drain the accumulated vertices so the next frame starts clean — without
	// this lines from one frame would persist if the caller stops adding.
	private void Render( SceneObject self )
	{
		int count;
		int segCount;
		int faceCount;
		lock ( _lock )
		{
			count = _lineVerts.Count;
			faceCount = _faceVerts.Count;
			if ( count == 0 && faceCount == 0 ) return;
			segCount = _lineThicknesses.Count;

			// Copy out and clear BEFORE drawing. Graphics.Draw may hold the
			// buffer past return for deferred submission, so anything we hand
			// it must not be touched again this frame.
			if ( _drawBuffer.Length < count )
				_drawBuffer = new Vertex[Math.Max( count, _drawBuffer.Length * 2 )];
			if ( _drawThickness.Length < segCount )
				_drawThickness = new float[Math.Max( segCount, _drawThickness.Length * 2 )];
			if ( _faceBuffer.Length < faceCount )
				_faceBuffer = new Vertex[Math.Max( faceCount, _faceBuffer.Length * 2 )];
			_lineVerts.CopyTo( _drawBuffer, 0 );
			_lineThicknesses.CopyTo( _drawThickness, 0 );
			_faceVerts.CopyTo( _faceBuffer, 0 );
			_lineVerts.Clear();
			_lineThicknesses.Clear();
			_faceVerts.Clear();
		}

		// Faces first so subsequent line draws (outlines, gizmos) render
		// crisply on top of the fill instead of being washed out by the
		// translucent overlay.
		if ( faceCount > 0 && _faceMaterial is not null )
		{
			Graphics.Draw( _faceBuffer, faceCount, _faceMaterial, null, Graphics.PrimitiveType.Triangles );
		}

		// Walk segments in order, grouping consecutive runs of identical
		// thickness into a single Graphics.Draw. Typical frames produce a
		// handful of groups (one per distinct thickness value); the per-group
		// staging copy is bounded by the largest group's vertex count.
		var i = 0;
		while ( i < count )
		{
			var segIdx = i / 2;
			var groupThickness = _drawThickness[segIdx];
			var groupStart = i;
			while ( i < count && _drawThickness[i / 2] == groupThickness )
			{
				i += 2;
			}
			var groupLen = i - groupStart;

			if ( _groupBuffer.Length < groupLen )
				_groupBuffer = new Vertex[Math.Max( groupLen, _groupBuffer.Length * 2 )];
			Array.Copy( _drawBuffer, groupStart, _groupBuffer, 0, groupLen );

			_attributes.Set( "LineThickness", groupThickness );
			Graphics.Draw( _groupBuffer, groupLen, _defaultLineMaterial, _attributes, Graphics.PrimitiveType.Lines );
		}
	}

	private static RenderAttributes _attributes = new RenderAttributes();

	// Caller must hold _lock. Inline Add appends directly to _lineVerts
	// without re-locking — the public Box overload acquires the lock once
	// around the whole 24-vert batch.
	private static void AddBoxEdges( BBox box, Color32 color, float thickness )
	{
		var min = box.Mins;
		var max = box.Maxs;
		var c000 = new Vector3( min.x, min.y, min.z );
		var c100 = new Vector3( max.x, min.y, min.z );
		var c110 = new Vector3( max.x, max.y, min.z );
		var c010 = new Vector3( min.x, max.y, min.z );
		var c001 = new Vector3( min.x, min.y, max.z );
		var c101 = new Vector3( max.x, min.y, max.z );
		var c111 = new Vector3( max.x, max.y, max.z );
		var c011 = new Vector3( min.x, max.y, max.z );

		Add( c000, c100 ); Add( c100, c110 ); Add( c110, c010 ); Add( c010, c000 );
		Add( c001, c101 ); Add( c101, c111 ); Add( c111, c011 ); Add( c011, c001 );
		Add( c000, c001 ); Add( c100, c101 ); Add( c110, c111 ); Add( c010, c011 );

		void Add( Vector3 a, Vector3 b )
		{
			Instance._lineVerts.Add( new Vertex( a ) { Color = color } );
			Instance._lineVerts.Add( new Vertex( b ) { Color = color } );
			Instance._lineThicknesses.Add( thickness );
		}
	}
}
