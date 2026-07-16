using HalfEdgeMesh;
using Sandbox.Movement;
using Sandbox.Navigation;
using Sandbox.UI;
using System;
using System.Linq;
using System.Reflection.Metadata;
using System.Threading.Tasks;
using static Sandbox.Storage;
using static Sandbox.VertexLayout;

public sealed class Player : Component
{
	#region Enums

	public enum Tool
	{
		None,
		Box,
		Sphere,
		Cylinder,
		Move,
		Rotate,
		Scale,
		Slice,
		Pick,
		// Face-mode UV manipulation tools: each operates on the current
		// _selectedFaces, picking the cursor onto each face's own plane,
		// and applies its operation (pan / scale / rotate) to the face's
		// per-face texture parameters while LMB is held.
		UVPan,
		UVScale,
		UVRotate,
	}

	public enum SelectionMode
	{
		Brush,
		Face,
		Vertex,
		Edge,
		Entity,
	}

	private enum SelectMode
	{
		Replace,
		Add,
		Remove,
	}

	private enum GizmoAxis { None, X, Y, Z, XY, YZ, XZ }

	private enum GizmoAxisSide { None, PosX, NegX, PosY, NegY, PosZ, NegZ }

	#endregion

	#region Constants

	private const int GridLength = 100;
	// Target visible extent for the editor grid, in world units. RebuildGrid
	// picks a line count so the grid covers ~this distance regardless of the
	// step size — otherwise shrinking the step shrinks the rendered area
	// and the grid appears to lower into the floor as you zoom in.
	private const float GridTargetExtent = 1600f;
	// Hard cap on lines per side so very small grid sizes can't blow up
	// vertex counts when the camera is moving.
	private const int GridMaxStepsPerSide = 400;
	private const float MaxRayDistance = 99999f;

	// Tag stamped onto any brush whose local AABB contains the camera, so
	// selection traces (HandleSelectionTrace / DrawHoveredBrushOutline /
	// texture pick) can skip its inner faces with WithoutTags. Refreshed
	// each frame in RefreshSelectionIgnoreTags. Kept off public tags to
	// avoid colliding with any user-defined ones.
	private const string SelectionIgnoreTag = "togethercsg-selection-ignore";

	private const float _gizmoArrowLength = 20f;
	private const float _gizmoHitRadius = 2.5f;
	private const float _gizmoHeadSize = 5f;
	private const float _gizmoHeadHalfRatio = 0.4f;
	// Line thickness passed through to CustomOverlay's LineThickness render
	// attribute. Gizmo lines (move arrows, rotate ring, scale handles) get
	// the heavy weight; everything else that renders outline-style (hover
	// edges, selection boxes, forward-arrow markers, slice indicator,
	// creation cursor box, ...) uses the lighter outline weight. Constants
	// are public so ModelBrush and any future overlay drawer can share the
	// same scale rather than each hard-coding a number.
	public const float GizmoLineThickness = 8f;
	public const float OutlineLineThickness = 3f;
	// Diagonal sub-axis bars bridge the midpoints of two adjacent main arrows
	// and drag freely in the plane defined by those two axes (both components
	// move at once, like a standard 3D-editor plane handle).
	private const float _gizmoDiagonalMidRatio = 0.5f;
	private const float _gizmoReferenceDistance = 100f;
	private const float _gizmoMinScale = 0.25f;
	// Exposed for ModelBrush's vertex-hover boxes so they pick up the same
	// camera-distance falloff as the move/rotate gizmos instead of rendering
	// at a fixed world size (tiny when zoomed out, huge when zoomed in).
	public const float GizmoReferenceDistance = _gizmoReferenceDistance;
	public const float GizmoMinScale = _gizmoMinScale;

	private const float _gizmoRingRadius = 25f;
	private const float _gizmoRingHitRadius = 4f;
	private const int _gizmoRingSegments = 48;

	private const float _gizmoScaleHandleSize = 2.5f;
	private const float _minScaleFactor = 0.01f;

	private const float _walkSpeedMultiplier = 0.5f;
	private const float _spawnDistanceFromCamera = 100f;
	private const float _initialEndpointOffset = 5f;
	private const float _fallbackGridStep = 1f;
	public const float MinGridSize = 1f;
	public const float MaxGridSize = 128f;

	// Fly-mode camera-relative controls. Both rates are scaled by the
	// current RunSpeed (using _flyReferenceSpeed as the unit baseline) so
	// the same gesture feels proportional whether the speed slider is on
	// "creep" or "warp".
	private const float _flyReferenceSpeed = 250f;
	private const float _panMouseSensitivity = 0.4f;     // world units per mouse pixel at baseline speed
	private const float _scrollWheelSensitivity = 1f;  // world units per scroll tick at baseline speed

	private const float _forwardArrowLength = 30f;
	private const float _forwardArrowHeadLength = 6f;
	private const float _forwardArrowHeadHalf = 3f;
	private const float _capUVScale = ModelBrush.DefaultUVScale;

	private const float _crossHelperZThreshold = 0.9f;
	private const float _planeCrossThreshold = 0.999f;
	private const float _axisEpsilon = 1e-6f;
	private const float _radialEpsilon = 1e-4f;

	private const float _sliceGizmoSize = 5f;
	private const float _sliceMinDragDistance = 1f;
	private const float _sliceTangentSpan = 100f;

	// Move-tool box-select tuning.
	// A press → release with cursor motion under this many pixels is treated
	// as a click (so the existing click-select path runs); above it engages
	// the marquee. Mirrors the 5px threshold the s&box editor uses.
	private const float _boxSelectMinDrag = 5f;
	// Near / far depths (units from the camera) of the marquee's selection
	// frustum. Near is small but non-zero so the side planes are well-formed
	// even when the screen rectangle is small; far is MaxRayDistance so the
	// volume extends to the same range the click-trace covers.
	private const float _boxSelectNearDepth = 1f;
	private const float _boxSelectFarDepth = MaxRayDistance;
	// Weld radius applied before AND after the slice's ClipFacesByPlaneAndCap.
	// Pre-cut covers non-manifold input (clone via MergeMesh, prior slice/boolean
	// leftovers); post-cut covers the per-face boundary duplicates FaceCutter
	// emits along the cut plane that break the cap loop.
	private const float _sliceWeldEpsilon = 0.01f;
	// "Vertex sits on the cut plane" tolerance for our manual cap synth. Needs
	// to comfortably exceed the post-weld averaging error so all boundary verts
	// get classified into the loop.
	private const float _sliceCapPlaneEpsilon = 0.02f;

	private const float _vertexSnapHitRadius = 6f;

	private static readonly Vector3 _entityPlaceholderColliderScale = new Vector3( 16f, 16f, 32f );
	// MAP-imported entities that aren't backed by a cloud package get a
	// simple cube placeholder — same size as the legacy Quake "point entity"
	// 16-unit box so imported `info_player_start` etc. read at familiar scale.
	private static readonly Vector3 _mapEntityPlaceholderColliderScale = new Vector3( 16f, 16f, 16f );
	private static readonly Color _mapEntityOutlineColor = Color.Cyan;

	public static readonly float[] AngleSnapValues = { 1.40625f, 2.8125f, 5.625f, 11.25f, 22.5f, 45f, 90f, 180f };

	#endregion

	#region Static

	public static Player Instance { get; private set; }

	private static readonly Color _firstColor = Color.Red;
	private static readonly Color _secondColor = Color.Green;

	private static readonly Color _gizmoXColor = Color.Red;
	private static readonly Color _gizmoYColor = Color.Green;
	private static readonly Color _gizmoZColor = Color.Blue;
	// Diagonal sub-axis colors blend the two axes they bridge — yellow for
	// the X-Y bisector (red + green), cyan for Y-Z (green + blue), magenta
	// for X-Z (red + blue).
	private static readonly Color _gizmoXYColor = Color.Lerp( _gizmoXColor, _gizmoYColor, 0.5f );
	private static readonly Color _gizmoYZColor = Color.Lerp( _gizmoYColor, _gizmoZColor, 0.5f );
	private static readonly Color _gizmoXZColor = Color.Lerp( _gizmoXColor, _gizmoZColor, 0.5f );
	private static readonly Color _gizmoHighlightColor = Color.Yellow;

	private static readonly Color _sliceGizmoColor = Color.Green;

	// Selection-fill tint used by CustomOverlay.Face on top of the existing
	// yellow outline. Translucent so the underlying texture still reads
	// through; matches the alpha s&box's FaceTool uses for its overlay.
	private static readonly Color _selectionFillColor = Color.Yellow.WithAlpha( 0.1f );

	#endregion

	#region Fields

	// Group: General
	public FilePickerDialog _filePickerDialog;
	public CloudDialog _cloudDialog;
	public Toolbar _toolbar;
	public SHLightVolume _shLightVolume;
	public DirectionalLight _sunLight;
	// Most recently loaded or saved .map path. Used to default the LOAD /
	// SAVE dialogs to wherever the user last worked, instead of pinning
	// SAVE to "untitled.map" at the data root every time. Per-session
	// only — not persisted to disk.
	private string _lastMapFilename = "";
	private CloudInstance _placingCloud;
	[Property]
	public Vector3 PlaneNormal;
	[Property]
	public float PlaneDistance;
	[Property]
	private GameObject _cubePrefab;
	[Property]
	private GameObject _spherePrefab;
	[Property]
	private GameObject _cylinderPrefab;
	[Property]
	private Material _lineMaterial;
	[Property]
	private Material _templateMaterial;

	[Property]
	private MoveModeWalk _moveModeWalk;

	[Property]
	private FlyMove _flyMove;

	private readonly UndoSystem _undo = new();
	// Default grid extent in world units. Picked to land at a sensible
	// Quake-style 64-unit cube edge — small enough to navigate, large enough
	// that fresh primitives don't sit on a sub-unit grid. The [Property]
	// setter still overrides this from scene data, so loaded maps keep
	// whatever they stored.
	private float _gridSize = 64f;
	private bool _hidden;
	private bool _gridEnabled = true;
	// Per-player visibility filter: when a brush is in this set its
	// MeshComponent is force-disabled so the local client neither renders
	// nor raycasts against it. Other peers see the brush normally — hide
	// is a view filter, not an edit.
	private readonly HashSet<ModelBrush> _hiddenBrushes = new();
	// Brushes currently carrying SelectionIgnoreTag. Tracked so we can
	// clear last frame's stamps cleanly before recomputing this frame's.
	private readonly HashSet<ModelBrush> _selectionIgnoreTagged = new();

	// Group: Tool / Selection State
	// BOX is the default so the toolbar boots into "draw a brush" rather
	// than an empty-tool state — a zero-area click in this mode is
	// repurposed as a selection click (see the Box/Sphere/Cylinder case
	// in OnUpdate), so users can still click-to-pick without first
	// switching tools.
	private Tool _tool = Tool.Box;
	private SelectionMode _selection;
	private readonly HashSet<ModelBrush> _selectedBrushes = new();
	private readonly HashSet<(ModelBrush, int, uint)> _selectedEdges = new();
	private readonly HashSet<(ModelBrush, int)> _selectedFaces = new();
	// Vertex selection is keyed by the polygon mesh vertex index so it
	// survives mesh rebuilds (we remap via MoveVerticesLocal). The old
	// (triangle, triangleVertexIndex) tuple wasn't a stable identity since
	// a single vertex is shared across triangles.
	// Stored as an ordered list (not a HashSet) so the first-selected vertex
	// is well-defined: it's the snap anchor on multi-vertex drags, and the
	// move gizmo sits on it. ApplySelectOrdered keeps the toggle semantics
	// the other selection sets get from HashSet's Add.
	private readonly List<(ModelBrush, int)> _selectedVertices = new();
	private readonly HashSet<CloudInstance> _selectedCloud = new();
	// Brushes the user had selected when they entered Vertex mode — these
	// are the ONLY brushes whose vertices can be picked by clicking. Snap-
	// during-drag (SnapVertexWorldPosition) still considers every brush.
	private readonly HashSet<ModelBrush> _vertexContextBrushes = new();

	// Group: Grid
	private readonly List<Vertex> _gridLines = new();
	private SceneCustomObject _grid;
	private Vector3 _gridSnappedCenter;

	// Group: Primitive Drawing
	private Vector3 _startPoint;
	private Vector3 _endPoint;
	private float _height;
	private bool _scalingUp;
	private Vector3 _firstStepPlanePoint;
	private Vector3 _firstStepPlaneNormal;

	// Group: Box-select (Move tool marquee)
	// Screen-space anchor and cursor positions of the active marquee.
	// _boxSelectArmed flips on LMB-press in the Move tool when no gizmo
	// consumed the click; _boxSelecting flips once the cursor travels far
	// enough to engage the marquee (so a click-and-release without drag
	// stays a click-select).
	private Vector2 _boxSelectStartScreen;
	private Vector2 _boxSelectCurrentScreen;
	private bool _boxSelectArmed;
	private bool _boxSelecting;

	// Group: Slice
	private Vector3 _sliceStartPoint;
	private Vector3 _sliceCurrentPoint;
	private bool _sliceDragging;
	private bool _sliceHasPick;
	private Vector3 _slicePickPoint;
	private Vector3 _slicePickNormal;
	private Vector3 _sliceSurfaceNormal;
	private Plane _sliceDragSurfacePlane;

	// Group: Face UV tools (Tool.UVPan / .UVScale / .UVRotate)
	// One entry per selected face, captured at BeginUVOp. We ray-cast onto
	// each face's own plane (its world normal + a vertex as plane point) on
	// LMB-press, snapshot the existing UV params, then update them per-tick
	// while LMB is held: pan moves the offset by the world drag projected
	// onto the face's stored world-space U/V axes; scale multiplies the
	// face's scale by the radial distance change from press to cursor
	// around the face centroid; rotate spins axisU/axisV around the face
	// normal by the angle the cursor has swept around the centroid.
	private enum UVOpKind { None, Pan, Scale, Rotate }
	private struct UVOpFaceState
	{
		public ModelBrush Brush;
		public int Triangle;
		public Vector2 StartOffset;
		public Vector2 StartScale;
		public Vector4 StartAxisU;
		public Vector4 StartAxisV;
		public Vector3 StartHit;
		public Vector3 AxisU;       // .xyz of StartAxisU
		public Vector3 AxisV;
		public Vector3 PlanePoint;
		public Vector3 PlaneNormal;
		public Vector3 FaceCentroidWorld;
	}
	private readonly List<UVOpFaceState> _uvOpFaces = new();
	private UVOpKind _uvOpActive = UVOpKind.None;
	// Rotate-specific state. Captures _uvRotation at press time so the
	// tool can drive the same scalar the UV ROTATION NumberEntry binds
	// to, and tracks the cursor's accumulated angle around a single
	// reference face so multi-revolution drags rotate by more than a
	// full circle (raw atan2 would wrap each frame).
	private float _uvRotateStartRotation;
	private float _uvRotateAccumulatedDeg;
	private float _uvRotatePrevAngleDeg;
	private bool _uvRotateRefValid;
	private Vector3 _uvRotateRefCentroid;
	private Vector3 _uvRotateRefPlanePoint;
	private Vector3 _uvRotateRefPlaneNormal;
	private Vector3 _uvRotateRefAxisU;
	private Vector3 _uvRotateRefAxisV;

	// Group: Gizmo / Rotate / Scale state
	private bool _gizmoVisible;
	private Vector3 _gizmoCenter;
	private float _gizmoScale = 1f;
	private GizmoAxis _gizmoHoveredAxis;
	private GizmoAxis _gizmoActiveAxis;
	private Vector3 _gizmoDragStart;
	private Vector3 _gizmoDragAxisDir;
	private readonly Dictionary<ModelBrush, Vector3> _gizmoDragStartPositions = new();
	private readonly Dictionary<CloudInstance, Vector3> _gizmoDragStartPositionsCloud = new();
	// During a vertex Move drag we track each selected vertex's local position
	// at drag-start so subsequent frames recompute "start + translation"
	// against a stable anchor. The keyed (brush, vertexIndex) pair is updated
	// after each MoveVerticesLocal because the rebuild reassigns indices.
	private readonly Dictionary<(ModelBrush brush, int vertexIndex), Vector3> _vertexDragStartLocal = new();
	// Selected vertex closest to the cursor ray at drag-start. Vertex-snap only
	// snaps THIS vertex; every other selected vertex follows by the same
	// translation so the multi-vertex selection moves as a rigid set instead of
	// each vertex independently snapping to its own nearest neighbour (which
	// previously pulled the selection apart).
	private (ModelBrush brush, int vertexIndex)? _vertexDragAnchor;
	private bool _gizmoShiftPendingClone;

	private bool _rotGizmoVisible;
	private Vector3 _rotGizmoCenter;
	private float _rotGizmoScale = 1f;
	private GizmoAxis _rotHoveredAxis;
	private GizmoAxis _rotActiveAxis;
	private Vector3 _rotDragStartDir;
	private Vector3 _rotDragPivot;
	// Local-gizmo rotation drag bakes the brush rotation into the gizmo
	// axes — but rotating the brush during the drag would then drag the
	// gizmo with it and feed back into itself. Cache the basis used at
	// drag-start so the ring stays anchored in world space for the
	// duration of the drag.
	private Rotation? _rotDragBasisOverride;
	private readonly Dictionary<ModelBrush, (Vector3 Position, Rotation Rotation)> _rotDragStartTransforms = new();
	private readonly Dictionary<CloudInstance, (Vector3 Position, Rotation Rotation)> _rotDragStartTransformsCloud = new();

	private bool _scaleGizmoVisible;
	private float _scaleGizmoScale = 1f;
	private GizmoAxisSide _scaleHoveredSide;
	private GizmoAxisSide _scaleActiveSide;
	private Vector3 _scaleCenter;
	private readonly Vector3[] _scaleAxes = new Vector3[3];
	private readonly float[] _scaleExtents = new float[3];
	private Vector3 _scaleDragAxisDir;
	private Vector3 _scaleDragPivot;
	private float _scaleStartSize;
	private float _scaleCurrentFactor = 1f;
	private readonly Dictionary<ModelBrush, (Vector3 Position, Vector3 Scale)> _scaleDragStartTransforms = new();
	private readonly Dictionary<CloudInstance, (Vector3 Position, Vector3 Scale)> _scaleDragStartTransformsCloud = new();

	// Group: Texture/UV
	private string _selectedTexturePath;
	private float _uvRotation = 0f;
	private float _angleSnap = 22.5f;

	// Group: Play-mode entity spawning
	private readonly List<GameObject> _playModeEntities = new();
	private readonly HashSet<CloudInstance> _playModeHiddenMarkers = new();

	#endregion

	#region Properties

	// Group: General
	[Property]
	public PlayerController _playerController { get; private set; }

	// The host clones the Player prefab per connection, so every client has a
	// proxy Player for each remote peer that must stay idle.
	private bool IsLocalPlayer => _playerController is not null && !_playerController.IsProxy;

	public bool IsPlayMode { get; set; }
	public bool IsBaking;

	public Material TemplateMaterial => _templateMaterial;

	public float GridSize
	{
		get => _gridSize;
		set
		{
			_gridSize = value;
			RebuildGrid();
		}
	}

	public float RunSpeed
	{
		get => _playerController?.RunSpeed ?? 0f;
		set
		{
			if ( _playerController is null ) return;
			_playerController.RunSpeed = value;
			_playerController.WalkSpeed = value * _walkSpeedMultiplier;
		}
	}

	// Group: Tool / Selection State
	public Tool CurrentTool
	{
		get => _tool;
		set
		{
			if ( _tool == value ) return;
			_tool = value;
			// Tool switches abandon any in-progress Move-tool marquee — its
			// start screen position would otherwise rebind to the new tool's
			// first cursor sample on re-entry. UV pan is also Move-only and
			// gets committed so the texture changes so far don't dangle.
			_boxSelectArmed = false;
			_boxSelecting = false;
			if ( _uvOpActive != UVOpKind.None ) EndUVOp( commit: true );
		}
	}

	public SelectionMode Selection
	{
		get => _selection;
		private set
		{
			if ( _selection == value ) return;
			_selection = value;
			ClearAllSelections();
			// Vertex-mode context is meaningful only while in Vertex mode;
			// drop it on every mode change so re-entering Vertex doesn't
			// inherit stale context brushes.
			_vertexContextBrushes.Clear();
			_boxSelectArmed = false;
			_boxSelecting = false;
			if ( _uvOpActive != UVOpKind.None ) EndUVOp( commit: true );
		}
	}

	public bool HasFacesSelected => _selectedFaces.Count > 0;

	// Boolean ops abort inside SubtractionAction when no selected brush is
	// editable; mirror that here so the toolbar greys out instead of firing
	// a click that silently does nothing.
	private bool AllSelectedBrushesEditable => _selectedBrushes.All( b => b.IsValid() && b.CanLocalEdit() );

	public bool CanUnion => Selection == SelectionMode.Brush && _selectedBrushes.Count >= 2 && AllSelectedBrushesEditable;
	// Subtract carves each selected brush out of every other brush whose
	// world bounds it overlaps, so a single source brush is enough.
	public bool CanSubtract => Selection == SelectionMode.Brush && _selectedBrushes.Count >= 1 && AllSelectedBrushesEditable;
	public bool CanIntersect => Selection == SelectionMode.Brush && _selectedBrushes.Count == 2 && AllSelectedBrushesEditable;

	// When true, dragging a vertex snaps it onto any non-moving vertex within
	// _vertexSnapHitRadius (or half the grid step). When false, only grid
	// alignment is applied. Only meaningful in Vertex selection mode.
	public bool VertexSnap { get; set; } = true;

	// When true, picking a vertex in Vertex mode also picks every other vertex
	// in the selected (context) brushes whose world position is within
	// _mergedSelectionRadius of it — the "merged" vertex picker. The same
	// expansion applies on deselect. Lets the user move a shared corner of
	// several adjacent brushes as a single welded handle. SnapVertexWorld-
	// Position already skips every vertex currently being dragged, so the
	// anchor never snaps onto its own merged siblings either.
	public bool MergedSelection { get; set; } = true;
	private const float _mergedSelectionRadius = 0.01f;

	// When true, the CustomOverlay.Face translucent-fill pass runs on every
	// brush / face / slice target / vertex-context brush that the selection
	// rendering would otherwise just outline. Toggleable from the Texture/UV
	// "MASK" button on the right panel; default true. Turn off to fall back
	// to outline-only when the fill is too noisy on top of textured faces.
	public bool SelectionMask { get; set; } = true;

	// Segment counts the toolbar passes to newly-spawned sphere / cylinder
	// brushes. Defaults match the brush classes' built-in defaults so the
	// behaviour out-of-the-box is identical to before this feature
	// existed. Clamped to the brush classes' minimums on assignment so a
	// bad text-entry value can't produce a degenerate mesh.
	public const int DefaultSphereSegments = SphereModelBrush.DefaultSlices;
	public const int DefaultCylinderSegments = CylinderModelBrush.DefaultSlices;

	private int _sphereSegments = DefaultSphereSegments;
	private int _cylinderSegments = DefaultCylinderSegments;

	public int SphereSegments
	{
		get => _sphereSegments;
		set => _sphereSegments = Math.Clamp( value, SphereModelBrush.MinSlices, SphereModelBrush.MaxSlices );
	}

	public int CylinderSegments
	{
		get => _cylinderSegments;
		set => _cylinderSegments = Math.Clamp( value, CylinderModelBrush.MinSlices, CylinderModelBrush.MaxSlices );
	}

	// When true, the Move and Rotate gizmos align to the selected shape's
	// local axes instead of world axes. Per-player view setting; doesn't
	// touch the brush data itself.
	public bool LocalGizmo { get; set; }

	// LOCAL only affects the gizmo when there's a single shape whose
	// rotation can stand in for the gizmo basis (see GizmoBasis). For
	// multi-selection or empty selection it would be a no-op, so the
	// toolbar greys the button out using this gate.
	public bool LocalGizmoApplies =>
		(Selection == SelectionMode.Brush && _selectedBrushes.Count == 1)
		|| (Selection == SelectionMode.Entity && _selectedCloud.Count == 1);

	// Master toggle for the grid: when off, AlignToGrid / SnapAngle are
	// no-ops and the grid plane stops rendering. Grid size is preserved so
	// re-enabling restores the user's previous spacing.
	public bool GridEnabled
	{
		get => _gridEnabled;
		set
		{
			if ( _gridEnabled == value ) return;
			_gridEnabled = value;
			RebuildGrid();
		}
	}

	// HIDE button needs at least one brush selected in Brush mode. Hidden
	// brushes stay hidden until UNHIDE ALL is clicked.
	public bool CanHide => !IsPlayMode && Selection == SelectionMode.Brush && _selectedBrushes.Count > 0;
	public bool HasHiddenBrushes => _hiddenBrushes.Count > 0;
	public bool CanUnhideAll => !IsPlayMode && HasHiddenBrushes;

	// True while the PICK tool is active: the next click on a textured face
	// copies its material path into SelectedTexturePath and turns the tool
	// off. Backed by CurrentTool so selecting any other tool deselects PICK
	// (and vice-versa) without extra wiring.
	public bool IsPickingTexture => CurrentTool == Tool.Pick;

	// Group: Texture/UV
	public bool LockUVs { get; set; }

	public float AngleSnap
	{
		get => _angleSnap;
		set
		{
			var clamped = value;
			if ( clamped < AngleSnapValues[0] ) clamped = AngleSnapValues[0];
			if ( clamped > AngleSnapValues[^1] ) clamped = AngleSnapValues[^1];
			_angleSnap = clamped;
		}
	}

	public bool CanIncreaseAngleSnap => Array.IndexOf( AngleSnapValues, _angleSnap ) < AngleSnapValues.Length - 1;
	public bool CanDecreaseAngleSnap => Array.IndexOf( AngleSnapValues, _angleSnap ) > 0;

	public string SelectedTexturePath
	{
		get
		{
			return _selectedTexturePath;
		}
		set
		{
			_selectedTexturePath = value;
			var preview = _toolbar?.TexturePreviewRef;
			if ( preview is not null )
			{
				preview.Texture = SelectedTexture;
				preview.Style.Dirty();
			}
		}
	}

	public Texture SelectedTexture =>
		!string.IsNullOrEmpty( SelectedTexturePath ) && GameNetwork.TryGetTexture( SelectedTexturePath, out var t )
			? t
			: null;

	public float UVTileX
	{
		get => ReadUVAgreement( ( mesh, face ) => mesh.GetTextureScale( face ).x );
		set => WriteUVPerFace( ( mesh, face ) =>
		{
			var s = mesh.GetTextureScale( face );
			s.x = value;
			mesh.SetTextureScale( face, s );
		} );
	}

	public float UVTileY
	{
		get => ReadUVAgreement( ( mesh, face ) => mesh.GetTextureScale( face ).y );
		set => WriteUVPerFace( ( mesh, face ) =>
		{
			var s = mesh.GetTextureScale( face );
			s.y = value;
			mesh.SetTextureScale( face, s );
		} );
	}

	public float UVOffsetX
	{
		get => ReadUVAgreement( ( mesh, face ) => mesh.GetTextureOffset( face ).x );
		set => WriteUVPerFace( ( mesh, face ) =>
		{
			var o = mesh.GetTextureOffset( face );
			o.x = value;
			mesh.SetTextureOffset( face, o );
		} );
	}

	public float UVOffsetY
	{
		get => ReadUVAgreement( ( mesh, face ) => mesh.GetTextureOffset( face ).y );
		set => WriteUVPerFace( ( mesh, face ) =>
		{
			var o = mesh.GetTextureOffset( face );
			o.y = value;
			mesh.SetTextureOffset( face, o );
		} );
	}

	public float UVRotation
	{
		get => _uvRotation;
		set
		{
			// Do NOT pass through SnapAngle — texture-UV rotation is independent
			// of the brush rotate gizmo's angle snap, so typing 17.3 here stays
			// 17.3 even when the rotate-snap is 22.5°.
			var normalized = ((value % 360f) + 360f) % 360f;
			if ( _uvRotation == normalized ) return;
			var deltaDeg = normalized - _uvRotation;
			_uvRotation = normalized;
			RotateSelectedFacesTangent( deltaDeg );
		}
	}

	// Group: Toolbar / Capability
	public bool CanUseRotateScaleTool => Selection != SelectionMode.Vertex && Selection != SelectionMode.Face;

	// Scale separately: vertex selection of 2+ also works (the gizmo bounds
	// the picked vertices' world AABB and the drag scales each vertex's
	// world position relative to the opposite handle, mirroring the
	// brush-scale formula). Face selection still doesn't get a frame.
	public bool CanUseScaleTool =>
		Selection == SelectionMode.Brush
		|| Selection == SelectionMode.Entity
		|| (Selection == SelectionMode.Vertex && _selectedVertices.Count >= 2);

	// Clone/Delete act on whole brushes (or whole entities); in Face/Edge/
	// Vertex modes the selection is a sub-component of an existing brush, so
	// the buttons are greyed out — duplicating or removing individual faces /
	// edges / verts via these actions isn't supported.
	public bool CanCloneOrDelete => Selection != SelectionMode.Vertex
		&& Selection != SelectionMode.Edge
		&& Selection != SelectionMode.Face;

	// Entering Vertex mode requires a brush context, so the button is only
	// enabled when at least one brush is currently selected (Brush mode),
	// or the user is already in Vertex mode (so the button can still show
	// pressed state).
	public bool CanUseVertexMode =>
		Selection == SelectionMode.Vertex
		|| (Selection == SelectionMode.Brush && _selectedBrushes.Count > 0);

	// Only the server host may import a map — clients see the toolbar button
	// disabled and the runtime call short-circuits here as a defense in depth.
	// Export stays available to everyone since it only writes a local file.
	public bool CanImport => !Networking.IsActive || Networking.IsHost;

	public bool CanSlice => Selection == SelectionMode.Brush && _selectedBrushes.Count >= 1 && _selectedBrushes.All( b => b.IsValid() );

	public bool CanMirror => !IsPlayMode && Selection == SelectionMode.Brush && _selectedBrushes.Count > 0
		&& _selectedBrushes.All( b => b.IsValid() && b.CanLocalEdit() );

	public bool CanUndo => _undo.CanUndo;
	public bool CanRedo => _undo.CanRedo;

	// Group: Gizmo scaled metrics
	private float ScaledArrowLength => _gizmoArrowLength * _gizmoScale;
	private float ScaledHeadSize => _gizmoHeadSize * _gizmoScale;
	private float ScaledHitRadius => _gizmoHitRadius * _gizmoScale;
	private float ScaledDiagonalMid => _gizmoArrowLength * _gizmoDiagonalMidRatio * _gizmoScale;

	private float ScaledRingRadius => _gizmoRingRadius * _rotGizmoScale;
	private float ScaledRingHitRadius => _gizmoRingHitRadius * _rotGizmoScale;

	private float ScaledScaleHandleSize => _gizmoScaleHandleSize * _scaleGizmoScale;
	private float ScaledScaleHitRadius => _gizmoHitRadius * _scaleGizmoScale;

	#endregion

	#region Public Helpers

	public GameObject GetSpawnPrefab() => _cubePrefab;

	#endregion

	[Property]
	private Dresser _dresser;

	#region Lifecycle

	protected override void OnAwake()
	{
		base.OnAwake();
		Mouse.Visibility = MouseVisibility.Hidden;
		// Grid SceneCustomObject is created in OnStart once IsProxy is reliable.
		_filePickerDialog = Scene.Directory.FindByName( "Screen" ).FirstOrDefault()?.GetComponent<FilePickerDialog>();
		_cloudDialog = Scene.Directory.FindByName( "Screen" ).FirstOrDefault()?.GetComponent<CloudDialog>();
		_toolbar = Scene.Directory.FindByName( "Screen" ).FirstOrDefault()?.GetComponent<Toolbar>();
		_shLightVolume = Scene.Directory.FindByName( "LightVolume" ).FirstOrDefault()?.GetComponent<SHLightVolume>();
		_sunLight = Scene.Directory.FindByName( "Sun" ).FirstOrDefault()?.GetComponent<DirectionalLight>();
		_ = _dresser.Apply();
	}

	protected override void OnStart()
	{
		base.OnStart();
		if ( !IsLocalPlayer ) return;
		Instance = this;
		GameNetwork.RebuildPendingMaterials();
		RebuildGrid();
		_grid = new SceneCustomObject( Scene.SceneWorld )
		{
			RenderOverride = RenderGrid,
		};
	}

	protected override void OnDestroy()
	{
		base.OnDestroy();
		_grid?.Delete();
		_grid = null;
		if ( Instance == this ) Instance = null;
	}

	protected override void OnUpdate()
	{
		// Proxy Players would otherwise capture input and draw duplicate gizmos.
		if ( !IsLocalPlayer ) return;

		PruneInvalidSelections();
		EnforceHiddenBrushes();
		BlurFocusedTextEntriesOnOutsideClick();

		if ( IsBaking ) return;

		DrawEntityForwardArrows();

		if ( _placingCloud is not null && !IsPlayMode )
		{
			var placeRay = Scene.Camera.ScreenPixelToRay( Mouse.Position );
			DrawHoveredBrushOutline( placeRay );
			UpdateCloudPlacement( placeRay );

			if ( Input.Pressed( "attack1" ) )
			{
				CommitCloudPlacement();
				return;
			}
			if ( Input.Pressed( "menu" ) )
			{

				CommitCloudPlacement();
				ToggleInterface();
				return;
			}
			return;
		}

		HandleKeyboardShortcuts();
		HandleFlyControls();
		UpdateGridFollow();
		DrawConcaveBrushOutlines();

		switch ( Selection )
		{
			case SelectionMode.Brush:
				foreach ( var selected in _selectedBrushes )
				{
					selected.Hover( true );
					DrawSelectedBrushFill( selected );
				}
				break;
			case SelectionMode.Edge:
				foreach ( var selected in _selectedEdges )
				{
					selected.Item1.HoverEdge( selected.Item2, selected.Item3, true );
				}
				break;
			case SelectionMode.Vertex:
				// Outline AND fill the context brushes (the ones picked while
				// in Brush mode) so the user can see which brushes are
				// pickable for vertex selection.
				foreach ( var b in _vertexContextBrushes )
				{
					if ( b is null || !b.IsValid() ) continue;
					b.Hover( true );
					DrawSelectedBrushFill( b );
				}
				foreach ( var selected in _selectedVertices )
				{
					selected.Item1.HoverVertexByIndex( selected.Item2, true );
				}
				break;
			case SelectionMode.Face:
				foreach ( var selected in _selectedFaces )
				{
					selected.Item1.HoverFace( selected.Item2, true );
					DrawSelectedFaceFill( selected.Item1, selected.Item2 );
				}
				break;
		}

		if ( Input.Pressed( "menu" ) )
		{
			if ( IsPlayMode )
			{
				TogglePlayModeAction();

			}
			else
			{
				ToggleInterface();
			}
		}

		if ( Input.Pressed( "test" ) )
		{

		}

		if ( Input.Down( "attack2" ) )
		{
			Mouse.Visibility = MouseVisibility.Hidden;
		}
		else
		{
			Mouse.Visibility = _hidden ? MouseVisibility.Hidden : MouseVisibility.Visible;
		}

		if ( Mouse.Visibility != MouseVisibility.Visible )
		{
			return;
		}

		if ( IsPlayMode ) return;

		UpdateGizmo();
		UpdateRotateGizmo();
		UpdateScaleGizmo();

		RefreshSelectionIgnoreTags();

		var ray = Scene.Camera.ScreenPixelToRay( Mouse.Position );

		switch ( CurrentTool )
		{
			case Tool.Move:
				// Once a marquee is armed we keep dispatch on the box-select
				// path: otherwise sweeping the cursor across the gizmo mid-
				// drag would let TraceGizmo consume the input and stall the
				// rubber-band visualisation. Gizmo gets first crack only on
				// the press frame, before the marquee has armed itself.
				if ( !_boxSelectArmed && UpdateGizmoDrag( ray ) ) return;
				if ( UpdateBoxSelect() ) return;
				HandleSelectionTrace( ray );
				return;
			case Tool.UVPan:
			case Tool.UVScale:
			case Tool.UVRotate:
				// Standalone UV-edit tools: LMB owns the operation, no
				// selection / hover trace runs so the cursor can't pick up
				// extra faces while the user is panning / scaling / rotating
				// UVs on the existing selection.
				UpdateUVTool( ray );
				return;
			case Tool.Rotate:
				if ( UpdateRotateGizmoDrag( ray ) ) return;
				HandleSelectionTrace( ray );
				return;
			case Tool.Scale:
				if ( UpdateScaleGizmoDrag( ray ) ) return;
				HandleSelectionTrace( ray );
				return;
			case Tool.Slice:
				UpdateSliceTool( ray );
				return;
			case Tool.Pick:
				UpdateTexturePicking( ray );
				return;
			case Tool.Box:
			case Tool.Cylinder:
			case Tool.Sphere:
				if ( !_scalingUp )
				{
					DrawHoveredBrushOutline( ray );
					if ( !Input.Down( "attack1" ) )
					{
						var brushTrace = Scene.SceneWorld.Trace.Ray( ray, MaxRayDistance ).WithoutTags( "player" ).Run();
						if ( brushTrace.Hit && brushTrace.SceneObject?.GetGameObject()?.GetComponent<ModelBrush>() != null )
						{
							var snap = brushTrace.HitPosition;
							AlignToGrid( ref snap );
							_firstStepPlanePoint = new Vector3( 0f, 0f, snap.z );
							_firstStepPlaneNormal = Vector3.Up;
						}
						else
						{
							_firstStepPlanePoint = PlaneNormal * PlaneDistance;
							_firstStepPlaneNormal = PlaneNormal;
						}
					}
					var plane = new Plane( _firstStepPlanePoint, _firstStepPlaneNormal );
					var intersection = plane.IntersectLine( ray.Position, ray.Position + ray.Forward * MaxRayDistance );
					if ( intersection != null )
					{
						var intersectionValue = intersection.Value;
						AlignToGrid( ref intersectionValue );
						if ( Input.Pressed( "attack1" ) )
						{
							_height = 0f;
							_startPoint = intersectionValue;
						}
						else if ( Input.Down( "attack1" ) )
						{
							_endPoint = intersectionValue;
						}
						if ( Input.Released( "attack1" ) )
						{
							var firstStep = new BBox( _startPoint, _endPoint );
							if ( firstStep.Size.x <= 0f || firstStep.Size.y <= 0f )
							{
								// Zero-area click: treat as a selection click
								// instead of a silent cancel. Trace under the
								// cursor, swap the active selection for the
								// hit brush (if any), and drop into MOVE so
								// the user can immediately act on it without
								// first having to switch tools off Box.
								_scalingUp = false;
								Selection = SelectionMode.Brush;
								_selectedBrushes.Clear();
								var pickTrace = Scene.SceneWorld.Trace.Ray( ray, MaxRayDistance ).WithoutTags( "player" ).Run();
								if ( pickTrace.Hit )
								{
									var picked = pickTrace.SceneObject?.GetGameObject()?.GetComponent<ModelBrush>();
									if ( picked is not null ) _selectedBrushes.Add( picked );
								}
								CurrentTool = Tool.Move;
								return;
							}
							else
							{
								_scalingUp = true;
							}
						}
						else if ( !Input.Down( "attack1" ) )
						{
							// Scale the initial creation cursor by camera
							// distance so it stays roughly screen-constant
							// (same falloff the move / rotate / vertex
							// gizmos use). Without this it's a fixed 5-unit
							// world cube — invisible far away, oversized
							// when zoomed in.
							var camDist = Vector3.DistanceBetween( Scene.Camera.WorldPosition, intersectionValue );
							var camScale = MathF.Max( GizmoMinScale, camDist / GizmoReferenceDistance );
							_startPoint = intersectionValue;
							_endPoint = intersectionValue + Vector3.One * (_initialEndpointOffset * camScale);
						}
						var bbox = new BBox( _startPoint, _endPoint );
						CustomOverlay.Box( bbox, _firstColor, default, default, true, OutlineLineThickness );
					}
				}
				else
				{
					var plane = new Plane( _endPoint, -Scene.Camera.WorldTransform.Forward );
					var intersection = plane.IntersectLine( ray.Position, ray.Position + ray.Forward * MaxRayDistance );
					if ( intersection != null )
					{
						var intersectionValue = intersection.Value;
						var bbox = new BBox( _startPoint, _endPoint );
						_height = MathF.Max( 0f, intersectionValue.z - bbox.Maxs.z );
						bbox.Maxs.z += _height;
						AlignToGrid( ref bbox.Mins );
						AlignToGrid( ref bbox.Maxs );
						CustomOverlay.Box( bbox, _secondColor, default, default, true, OutlineLineThickness );
						if ( Input.Released( "attack1" ) )
						{
							if ( bbox.Size.z <= 0f )
							{
								_scalingUp = false;
								ClearActiveSelection();
								return;
							}

							GameObject prefab;
							switch ( CurrentTool )
							{
								case Tool.Box:
									prefab = _cubePrefab;
									break;
								case Tool.Sphere:
									prefab = _spherePrefab;
									break;
								case Tool.Cylinder:
									prefab = _cylinderPrefab;
									break;
								default:
									return;
							}
							if ( prefab is null )
							{
								_scalingUp = false;
								return;
							}
							_undo.BeginEdit();
							prefab = prefab.Clone();
							prefab.WorldPosition = bbox.Center;
							prefab.LocalScale = bbox.Size;
							if ( Networking.IsActive )
							{
								prefab.NetworkSpawn();
							}
							var newBrush = prefab.GetComponent<ModelBrush>();
							if ( newBrush is not null )
							{
								// Apply the toolbar's segment counts before
								// the default-UV pass so the UV recompute
								// runs against the final face set, not the
								// stale prefab-default mesh.
								if ( newBrush is SphereModelBrush sphereBrush && newBrush.CanLocalEdit() )
								{
									sphereBrush.RpcSetSegments( SphereSegments, SphereSegments );
								}
								else if ( newBrush is CylinderModelBrush cylinderBrush && newBrush.CanLocalEdit() )
								{
									cylinderBrush.RpcSetSlices( CylinderSegments );
								}
								newBrush.RpcApplyDefaultFaceUVs();
								if ( !string.IsNullOrEmpty( SelectedTexturePath ) && newBrush.CanLocalEdit() )
								{
									newBrush.RpcSetAllFaceMaterial( SelectedTexturePath );
								}
								Selection = SelectionMode.Brush;
								_selectedBrushes.Clear();
								_selectedBrushes.Add( newBrush );
								_undo.TrackNewBrush( newBrush );
							}
							_undo.EndEdit();
							_scalingUp = false;
						}
					}
				}
				return;
		}
	}

	#endregion

	#region Cloud

	public void OpenCloudAction()
	{
		if ( IsPlayMode ) return;
		if ( _cloudDialog is null ) return;
		if ( _placingCloud is not null ) CommitCloudPlacement();
		ClearAllSelections();
		_cloudDialog.Show( SpawnCloudPackage );
	}

	private void BeginCloudPlacement( CloudInstance ci )
	{
		if ( ci is null || !ci.IsValid() ) return;
		_placingCloud = ci;
		if ( ci.GameObject is not null )
		{
			ci.GameObject.Tags.Add( "placing" );
		}
		Selection = SelectionMode.Entity;
		CurrentTool = Tool.Move;
		_selectedCloud.Clear();
		_selectedCloud.Add( ci );
	}

	private void UpdateCloudPlacement( Ray ray )
	{
		if ( _placingCloud is null ) return;
		if ( !_placingCloud.IsValid() || _placingCloud.GameObject is null )
		{
			_placingCloud = null;
			return;
		}

		var trace = Scene.Trace.Ray( ray, MaxRayDistance ).WithoutTags( "player", "placing" ).Run();
		Vector3 pos;
		if ( trace.Hit )
		{
			pos = trace.HitPosition;
		}
		else
		{
			var plane = new Plane( PlaneNormal * PlaneDistance, PlaneNormal );
			var intersection = plane.IntersectLine( ray.Position, ray.Position + ray.Forward * MaxRayDistance );
			pos = intersection ?? (ray.Position + ray.Forward * _spawnDistanceFromCamera);
		}
		AlignToGrid( ref pos );
		_placingCloud.WorldPosition = pos;
	}

	private void CommitCloudPlacement()
	{
		var ci = _placingCloud;
		_placingCloud = null;
		if ( ci is null || !ci.IsValid() ) return;
		if ( ci.GameObject is not null )
		{
			ci.GameObject.Tags.Remove( "placing" );
		}

		_undo.BeginEdit();
		_undo.TrackNewCloud( ci );
		_undo.EndEdit();
	}

	private void DrawEntityForwardArrows()
	{
		if ( IsPlayMode ) return;
		foreach ( var ci in CloudInstance.All )
		{
			if ( !ci.IsValid() ) continue;
			if ( !ci.IsEntityPlaceholder ) continue;
			if ( ci.GameObject is null || !ci.GameObject.Enabled ) continue;
			DrawForwardArrow( ci.WorldPosition, ci.WorldRotation.Forward );
			// MAP entity placeholders have no sprite/model — outline the
			// collider so the user can still see, hover and click them.
			if ( ci.IsMapEntity ) DrawMapEntityBox( ci );
		}
	}

	private void DrawMapEntityBox( CloudInstance ci )
	{
		var half = _mapEntityPlaceholderColliderScale * 0.5f;
		var local = new BBox( -half, half );
		var transform = ci.WorldTransform;
		// 8 corners → 12 edges. Brute-force draw each edge; the AABB is
		// small enough that overlay-line cost is negligible.
		var c = new Vector3[8];
		for ( var i = 0; i < 8; i++ )
		{
			c[i] = transform.PointToWorld( new Vector3(
				(i & 1) != 0 ? local.Maxs.x : local.Mins.x,
				(i & 2) != 0 ? local.Maxs.y : local.Mins.y,
				(i & 4) != 0 ? local.Maxs.z : local.Mins.z ) );
		}
		var edges = new (int, int)[]
		{
			(0,1),(2,3),(4,5),(6,7),
			(0,2),(1,3),(4,6),(5,7),
			(0,4),(1,5),(2,6),(3,7),
		};
		foreach ( var (a, b) in edges )
		{
			CustomOverlay.Line( c[a], c[b], _mapEntityOutlineColor, 0f, default, true, OutlineLineThickness );
		}
	}

	private void DrawForwardArrow( Vector3 origin, Vector3 forward )
	{
		if ( forward.LengthSquared < _radialEpsilon ) return;
		forward = forward.Normal;
		var tip = origin + forward * _forwardArrowLength;
		var color = Color.Yellow;

		CustomOverlay.Line( origin, tip, color, 0f, default, true, OutlineLineThickness );

		var helper = MathF.Abs( forward.z ) > _crossHelperZThreshold ? Vector3.Right : Vector3.Up;
		var right = Vector3.Cross( forward, helper ).Normal;
		var up = Vector3.Cross( right, forward ).Normal;

		var baseP = tip - forward * _forwardArrowHeadLength;
		CustomOverlay.Line( tip, baseP + right * _forwardArrowHeadHalf, color, 0f, default, true, OutlineLineThickness );
		CustomOverlay.Line( tip, baseP - right * _forwardArrowHeadHalf, color, 0f, default, true, OutlineLineThickness );
		CustomOverlay.Line( tip, baseP + up * _forwardArrowHeadHalf, color, 0f, default, true, OutlineLineThickness );
		CustomOverlay.Line( tip, baseP - up * _forwardArrowHeadHalf, color, 0f, default, true, OutlineLineThickness );
	}

	public void TogglePlayModeAction()
	{
		IsPlayMode = !IsPlayMode;
		if ( _placingCloud is not null ) CommitCloudPlacement();
		//if ( !IsPlayMode ) return;
		ToggleInterface();
		ClearAllSelections();
		_gizmoActiveAxis = GizmoAxis.None;
		_rotActiveAxis = GizmoAxis.None;
		_scaleActiveSide = GizmoAxisSide.None;
		_scalingUp = false;
		if ( IsPlayMode )
		{
			Tags.Remove( "player" );
			Tags.Add( "solidPlayer" );
			_playerController.ThirdPerson = true;
			SpawnPlayModeEntities();
		}
		else
		{
			Tags.Remove( "solidPlayer" );
			Tags.Add( "player" );
			_playerController.ThirdPerson = false;
			DestroyPlayModeEntities();
		}
	}

	private async void SpawnPlayModeEntities()
	{
		_ = Scene.NavMesh.Generate( Scene.PhysicsWorld );

		foreach ( var ci in CloudInstance.All.ToList() )
		{
			if ( ci.Kind != CloudInstance.AssetKind.Entity ) continue;
			if ( !ci.GameObject.Enabled ) continue;
			if ( string.IsNullOrEmpty( ci.PackageIdent ) ) continue;

			ci.GameObject.Enabled = false;
			_playModeHiddenMarkers.Add( ci );

			try
			{
				var se = await ResourceLibrary.LoadAsync<ScriptedEntity>( ci.PackageIdent )
					  ?? await Cloud.Load<ScriptedEntity>( ci.PackageIdent );
				if ( se?.Prefab is null ) continue;
				if ( !IsPlayMode ) break;

				var go = SceneUtility.GetPrefabScene( se.Prefab ).Clone();
				go.WorldTransform = ci.WorldTransform;
				go.Enabled = true;
				_playModeEntities.Add( go );
			}
			catch ( Exception e )
			{
				Log.Warning( $"Play mode: failed to spawn entity '{ci.PackageIdent}': {e.Message}" );
			}
		}
	}

	private void DestroyPlayModeEntities()
	{
		foreach ( var go in _playModeEntities )
		{
			if ( go.IsValid() ) go.Destroy();
		}
		_playModeEntities.Clear();

		foreach ( var ci in _playModeHiddenMarkers )
		{
			if ( ci.IsValid() ) ci.GameObject.Enabled = true;
		}
		_playModeHiddenMarkers.Clear();
	}

	private async void SpawnCloudPackage( Package package )
	{
		if ( package is null ) return;
		if ( IsPlayMode ) return;

		var camTransform = Scene.Camera.WorldTransform;
		var spawnTransform = new Transform(
			camTransform.Position + camTransform.Forward * _spawnDistanceFromCamera,
			Rotation.Identity,
			1f );

		switch ( package.TypeName )
		{
			case "model":
				await SpawnCloudModel( package, spawnTransform );
				break;
			case "sent":
			case "entity":
				await SpawnCloudEntityPlaceholder( package, spawnTransform );
				break;
			default:
				Log.Warning( $"Cloud spawn: don't know how to spawn package type '{package.TypeName}'" );
				break;
		}
	}

	private static void ActivateSpawnedObject( GameObject go )
	{
		if ( Networking.IsActive )
		{
			go.NetworkSpawn( true, null );
		}
		else
		{
			go.Enabled = true;
		}
	}

	private async Task SpawnCloudModel( Package package, Transform transform )
	{
		var model = await ResourceLibrary.LoadAsync<Model>( package.FullIdent )
				 ?? await Cloud.Load<Model>( package.FullIdent );
		if ( model is null )
		{
			Log.Warning( $"Cloud spawn: failed to load model '{package.FullIdent}'" );
			return;
		}

		var go = new GameObject( false, package.Title );
		go.WorldTransform = transform;

		var modelRenderer = go.AddComponent<ModelRenderer>();
		modelRenderer.Model = model;

		var collider = go.AddComponent<BoxCollider>();
		collider.Scale = model.Bounds.Size;
		collider.Center = model.Bounds.Center;
		collider.ColliderFlags = ColliderFlags.IgnoreMass;

		var ci = go.AddComponent<CloudInstance>();
		ci.Kind = CloudInstance.AssetKind.Model;
		ci.PackageIdent = package.FullIdent;
		ci.Renderer = modelRenderer;

		ActivateSpawnedObject( go );

		BeginCloudPlacement( ci );
	}

	// Editor-only stand-in for a MAP entity we have no cloud package for
	// (e.g. legacy Quake `info_player_start`). 16×16×16 BoxCollider + a
	// wireframe overlay drawn each frame (see DrawMapEntityBox). The
	// component's CloudInstance carries the classname + properties so a
	// later export round-trips back to MAP exactly as it was loaded.
	private CloudInstance SpawnMapEntityPlaceholder( string className, IEnumerable<KeyValuePair<string, string>> properties, Transform transform )
	{
		var go = new GameObject( false, string.IsNullOrEmpty( className ) ? "entity" : className );
		go.WorldTransform = transform;

		var col = go.AddComponent<BoxCollider>();
		col.Scale = _mapEntityPlaceholderColliderScale;
		col.Center = Vector3.Zero;

		var ci = go.AddComponent<CloudInstance>();
		ci.Kind = CloudInstance.AssetKind.MapEntity;
		ci.ClassName = className ?? "";
		ci.PackageIdent = "";
		ci.ThumbUrl = "";
		ci.SetProperties( properties );

		ActivateSpawnedObject( go );
		return ci;
	}

	private Task SpawnCloudEntityPlaceholder( Package package, Transform transform )
	{
		var go = new GameObject( false, package.Title );
		go.WorldTransform = transform;

		var sprite = go.AddComponent<SpriteRenderer>();
		sprite.Opaque = true;
		Texture thumbTex = null;
		try
		{
			thumbTex = Texture.Load( package.Thumb );
		}
		catch ( Exception e )
		{
			Log.Warning( $"Cloud spawn: failed to load thumbnail '{package.Thumb}': {e.Message}" );
		}
		if ( thumbTex is not null )
		{
			sprite.Sprite = Sprite.FromTexture( thumbTex );
		}

		var col = go.AddComponent<BoxCollider>();
		col.Scale = _entityPlaceholderColliderScale;
		col.Center = Vector3.Zero;

		var ci = go.AddComponent<CloudInstance>();
		ci.Kind = CloudInstance.AssetKind.Entity;
		ci.PackageIdent = package.FullIdent;
		ci.ThumbUrl = package.Thumb;

		ActivateSpawnedObject( go );

		BeginCloudPlacement( ci );
		return Task.CompletedTask;
	}

	#endregion

	#region Grid Rendering

	private void RenderGrid( SceneObject self )
	{
		if ( IsBaking ) return;
		Graphics.Draw( _gridLines, _gridLines.Count, _lineMaterial, null, Graphics.PrimitiveType.Lines );
	}

	private void RebuildGrid()
	{
		_gridLines.Clear();
		if ( !_gridEnabled || _gridSize <= 0f ) return;
		// Pick the step count so the grid covers roughly the same world
		// distance whatever the spacing is — small grids therefore draw
		// more lines (down to GridMaxStepsPerSide) and the rendered
		// extent stays close to GridTargetExtent.
		var stepsF = MathF.Round( GridTargetExtent / _gridSize );
		var steps = (int)Math.Clamp( stepsF, 1, GridMaxStepsPerSide );
		var extent = steps * _gridSize;
		var cx = _gridSnappedCenter.x;
		var cy = _gridSnappedCenter.y;
		for ( var i = -steps; i <= steps; i++ )
		{
			var offset = i * _gridSize;
			_gridLines.Add( new Vertex( new Vector3( cx - extent, cy + offset, 0 ) ) );
			_gridLines.Add( new Vertex( new Vector3( cx + extent, cy + offset, 0 ) ) );
			_gridLines.Add( new Vertex( new Vector3( cx + offset, cy - extent, 0 ) ) );
			_gridLines.Add( new Vertex( new Vector3( cx + offset, cy + extent, 0 ) ) );
		}
	}

	private void UpdateGridFollow()
	{
		if ( _grid is null || !_gridEnabled || _gridSize <= 0f ) return;
		if ( Scene?.Camera is null ) return;
		var cam = Scene.Camera.WorldPosition;
		var snapped = new Vector3(
			MathF.Round( cam.x / _gridSize ) * _gridSize,
			MathF.Round( cam.y / _gridSize ) * _gridSize,
			0f );
		if ( snapped == _gridSnappedCenter ) return;
		_gridSnappedCenter = snapped;
		RebuildGrid();
	}

	private void AlignToGrid( ref Vector3 point )
	{
		if ( !_gridEnabled || _gridSize <= 0f ) return;
		point.x = MathF.Round( point.x / _gridSize ) * _gridSize;
		point.y = MathF.Round( point.y / _gridSize ) * _gridSize;
		point.z = MathF.Round( point.z / _gridSize ) * _gridSize;
	}

	// Per-player visibility filter: hidden brushes have their MeshComponent
	// force-disabled so they neither render nor get caught by scene
	// raycasts (selection, hover, snap). Network sync still drives the
	// brush data — other peers see the same brush untouched. We re-disable
	// every frame because edit paths (RebuildMeshComponent, transform sync,
	// snapshot RPC) flip Enabled back on without consulting this set.
	private void EnforceHiddenBrushes()
	{
		if ( _hiddenBrushes.Count == 0 ) return;
		foreach ( var brush in _hiddenBrushes )
		{
			if ( brush is null || !brush.IsValid() ) continue;
			var mc = brush.MeshComponent;
			if ( mc is not null && mc.Enabled ) mc.Enabled = false;
		}
	}

	// Stamp SelectionIgnoreTag on every brush whose local AABB contains the
	// camera position; clear the stamp from brushes no longer in that set.
	// Selection traces include SelectionIgnoreTag in their WithoutTags so the
	// inner faces of the brush the user is standing inside don't block
	// click-picking on geometry beyond it. AABB is exact for box brushes and
	// slightly pessimistic for sphere / cylinder (false positives in the AABB
	// corners outside the actual hull); the cost of a false positive is just
	// "you can't click that brush while standing in its bounding box corner",
	// which is mild — the camera-inside check is the typical "user is in a
	// room and the floor blocks every click" case.
	private void RefreshSelectionIgnoreTags()
	{
		foreach ( var brush in _selectionIgnoreTagged )
		{
			if ( brush is null || !brush.IsValid() ) continue;
			brush.GameObject?.Tags.Remove( SelectionIgnoreTag );
		}
		_selectionIgnoreTagged.Clear();

		var cam = Scene.Camera?.WorldPosition ?? Vector3.Zero;
		foreach ( var brush in ModelBrush.Brushes )
		{
			if ( brush is null || !brush.IsValid() ) continue;
			var go = brush.GameObject;
			if ( go is null ) continue;
			var local = brush.WorldTransform.PointToLocal( cam );
			var bounds = brush.Bounds;
			if ( local.x < bounds.Mins.x || local.x > bounds.Maxs.x ) continue;
			if ( local.y < bounds.Mins.y || local.y > bounds.Maxs.y ) continue;
			if ( local.z < bounds.Mins.z || local.z > bounds.Maxs.z ) continue;
			go.Tags.Add( SelectionIgnoreTag );
			_selectionIgnoreTagged.Add( brush );
		}
	}

	public void DecreaseGridAction()
	{
		if ( IsPlayMode ) return;
		GridSize = MathF.Max( MinGridSize, PreviousPowerOfTwo( GridSize ) );
	}

	public void IncreaseGridAction()
	{
		if ( IsPlayMode ) return;
		GridSize = MathF.Min( MaxGridSize, NextPowerOfTwo( GridSize ) );
	}

	// Step grid sizes through 1, 2, 4, 8, 16, ... — if GridSize ever lands
	// on a non-power-of-two value (e.g. from a saved scene or a one-off
	// edit), Increase/Decrease snap to the nearest power of two in the
	// chosen direction instead of doubling/halving the off-axis value.
	private static float NextPowerOfTwo( float v )
	{
		if ( v < 1f ) return 1f;
		var log = MathF.Log2( v );
		var ceil = MathF.Ceiling( log );
		// On an exact power of two, log == ceil, so we'd return v unchanged
		// — step one rung up.
		if ( MathF.Abs( ceil - log ) < 1e-5f ) ceil += 1f;
		return MathF.Pow( 2f, ceil );
	}

	private static float PreviousPowerOfTwo( float v )
	{
		if ( v <= 1f ) return 1f;
		var log = MathF.Log2( v );
		var floor = MathF.Floor( log );
		if ( MathF.Abs( log - floor ) < 1e-5f ) floor -= 1f;
		return MathF.Pow( 2f, floor );
	}

	// Middle-mouse drag pans the player along its camera right/up axes,
	// and the scroll wheel pushes the player along the camera forward
	// axis. Both rates scale with the RunSpeed slider so fast travel
	// gets a proportionally larger nudge from the same gesture.
	private void HandleFlyControls()
	{
		if ( IsPlayMode ) return;
		if ( _playerController is null ) return;
		if ( Scene?.Camera is null ) return;
		if ( IsTextEntryFocused() ) return;

		var speedScale = RunSpeed > 0f ? RunSpeed / _flyReferenceSpeed : 1f;
		var camTransform = Scene.Camera.WorldTransform;

		if ( Input.Down( "mouse3" ) )
		{
			var delta = Mouse.Delta;
			if ( delta.LengthSquared > 0f )
			{
				// Drag follows the cursor: cursor right means the world
				// appears to shift right (player moves left), cursor down
				// means world appears to shift down (player moves up).
				var pan = (-camTransform.Right * delta.x + camTransform.Up * delta.y) * _panMouseSensitivity * speedScale;
				_playerController.WorldPosition += pan;
			}
		}

		var wheel = Input.MouseWheel;
		if ( MathF.Abs( wheel.y ) > 0f )
		{
			var forwardStep = camTransform.Forward * (wheel.y * _scrollWheelSensitivity * speedScale);
			_playerController.WorldPosition += forwardStep;
		}
	}

	#endregion

	#region Selection

	// Flag every concave brush so the user can see at a glance which shapes
	// are invalid operands for the CSG operators. Only matters while
	// EXPERIMENTAL_BRUSH_CSG is the active CSG path — the mesh-CSG path
	// handles concave inputs fine, so don't bother painting them red.
#if EXPERIMENTAL_BRUSH_CSG
	private static readonly Color _concaveOutlineColor = Color.Red;

	private void DrawConcaveBrushOutlines()
	{
		foreach ( var brush in ModelBrush.Brushes )
		{
			if ( brush is null || !brush.IsValid() ) continue;
			if ( brush.IsConvex ) continue;
			brush.DrawConcaveOutline( _concaveOutlineColor );
		}
	}
#else
	private void DrawConcaveBrushOutlines() { }
#endif

	// Push every face of `brush` into the selection-fill overlay so each face
	// renders its CustomOverlay.FaceMaterial layer on top of the yellow line
	// outline DrawFaceOutline already draws. Used in Brush selection mode.
	// Gated on the SelectionMask toggle (right-panel MASK button) — when off,
	// every call site short-circuits and only the outline pass runs.
	private static void DrawSelectedBrushFill( ModelBrush brush )
	{
		if ( Instance is null || !Instance.SelectionMask ) return;
		if ( brush is null || !brush.IsValid() ) return;
		var mesh = brush.PolygonMesh;
		if ( mesh is null ) return;
		var xform = brush.WorldTransform;
		foreach ( var face in mesh.FaceHandles )
		{
			CustomOverlay.Face( mesh, face, xform, _selectionFillColor );
		}
	}

	// Like DrawSelectedBrushFill but for a single triangle-indexed face.
	// Used in Face selection mode: a selection entry is (brush, triangle),
	// triangle gets resolved to the FaceHandle that owns it.
	private static void DrawSelectedFaceFill( ModelBrush brush, int triangle )
	{
		if ( Instance is null || !Instance.SelectionMask ) return;
		if ( brush is null || !brush.IsValid() ) return;
		var mesh = brush.PolygonMesh;
		if ( mesh is null ) return;
		var face = mesh.TriangleToFace( triangle );
		CustomOverlay.Face( mesh, face, brush.WorldTransform, _selectionFillColor );
	}

	private void DrawHoveredBrushOutline( Ray ray, ModelBrush exclude = null )
	{
		var trace = Scene.SceneWorld.Trace.Ray( ray, MaxRayDistance ).WithoutTags( "player", "placing", SelectionIgnoreTag ).Run();
		if ( !trace.Hit ) return;
		var brush = trace.SceneObject?.GetGameObject()?.GetComponent<ModelBrush>();
		if ( brush is null || brush == exclude ) return;
		if ( _selectedBrushes.Contains( brush ) ) return;
		brush.HoverOutline( Color.Red );
	}

	private void HandleSelectionTrace( Ray ray )
	{
		var traceResult = Scene.SceneWorld.Trace.Ray( ray, MaxRayDistance ).WithoutTags( "player", SelectionIgnoreTag ).Run();
		var clicked = Input.Pressed( "attack1" );
		var mode = CurrentSelectMode();

		if ( Selection == SelectionMode.Entity )
		{
			var entityHit = Scene.Trace.Ray( ray, MaxRayDistance ).WithoutTags( "player", SelectionIgnoreTag ).Run();
			if ( entityHit.Hit )
			{
				var ci = FindCloudInstance( entityHit.GameObject );
				if ( ci is not null )
				{
					if ( clicked ) SelectedAddCloud( ci, mode );
					return;
				}
			}
			if ( clicked && mode == SelectMode.Replace )
			{
				_selectedCloud.Clear();
			}
			return;
		}

		if ( traceResult.Hit )
		{
			var brush = traceResult.SceneObject.GetGameObject().GetComponent<ModelBrush>();
			if ( brush != null )
			{
				if ( Selection == SelectionMode.Brush )
				{
					brush.Hover();
					if ( clicked ) SelectedAddBrush( brush, mode );
				}
				else if ( Selection == SelectionMode.Edge )
				{
					var edgeIndex =
						 traceResult.VertexInfluence.x < traceResult.VertexInfluence.y
							? (traceResult.VertexInfluence.x < traceResult.VertexInfluence.z ? 0 : 2)
							: (traceResult.VertexInfluence.y < traceResult.VertexInfluence.z ? 1 : 2);
					brush.HoverEdge( traceResult.HitTriangle, (uint)edgeIndex );
					if ( clicked ) SelectedAddEdge( brush, traceResult.HitTriangle, (uint)edgeIndex, mode );
				}
				else if ( Selection == SelectionMode.Vertex )
				{
					// Vertex picking is restricted to the context brushes
					// captured when the user entered Vertex mode. Hovers
					// and clicks on any other brush are ignored.
					if ( !_vertexContextBrushes.Contains( brush ) ) return;
					var triVertIdx =
						traceResult.VertexInfluence.x > traceResult.VertexInfluence.y
							? (traceResult.VertexInfluence.x > traceResult.VertexInfluence.z ? 0 : 2)
							: (traceResult.VertexInfluence.y > traceResult.VertexInfluence.z ? 1 : 2);
					if ( brush.TryGetVertexIndexAtTriangle( traceResult.HitTriangle, triVertIdx, out var vIdx ) )
					{
						brush.HoverVertexByIndex( vIdx );
						if ( clicked ) SelectedAddVertex( brush, vIdx, mode );
					}
				}
				else if ( Selection == SelectionMode.Face )
				{
					brush.HoverFace( traceResult.HitTriangle );
					if ( clicked ) SelectedAddFace( brush, traceResult.HitTriangle, mode );
				}
			}
		}
		else if ( clicked && mode == SelectMode.Replace )
		{
			ClearActiveSelection();
		}
	}

	private static SelectMode CurrentSelectMode()
	{
		if ( Input.Down( "Walk" ) ) return SelectMode.Remove;
		if ( Input.Down( "Run" ) ) return SelectMode.Add;
		return SelectMode.Replace;
	}

	private void ClearActiveSelection()
	{
		switch ( Selection )
		{
			case SelectionMode.Brush: _selectedBrushes.Clear(); break;
			case SelectionMode.Face: _selectedFaces.Clear(); break;
			case SelectionMode.Edge: _selectedEdges.Clear(); break;
			case SelectionMode.Vertex: _selectedVertices.Clear(); break;
			case SelectionMode.Entity: _selectedCloud.Clear(); break;
		}
	}

	private void ClearAllSelections()
	{
		_selectedBrushes.Clear();
		_selectedEdges.Clear();
		_selectedVertices.Clear();
		_selectedFaces.Clear();
		_selectedCloud.Clear();
	}

	private void HandleKeyboardShortcuts()
	{
		if ( IsTextEntryFocused() ) return;

		// Ctrl-modified shortcuts handled first and exclusively: stops a
		// single keypress like Ctrl+Y from firing both Redo (its Ctrl
		// shortcut) and a letter-bound action that happens to share the
		// same key.
		if ( Input.Down( "Ctrl" ) )
		{
			if ( Input.Pressed( "Undo" ) ) UndoAction();
			else if ( Input.Pressed( "Redo" ) ) RedoAction();
			return;
		}

		if ( Input.Pressed( "AddBox" ) ) AddBoxAction();
		else if ( Input.Pressed( "AddSphere" ) ) AddSphereAction();
		else if ( Input.Pressed( "AddCylinder" ) ) AddCylinderAction();
		else if ( Input.Pressed( "OpenCloud" ) ) OpenCloudAction();

		if ( Input.Pressed( "ToolMove" ) ) MoveAction();
		else if ( Input.Pressed( "ToolRotate" ) ) RotateAction();
		else if ( Input.Pressed( "ToolScale" ) ) ScaleAction();
		else if ( Input.Pressed( "ToolSlice" ) ) SliceAction();

		if ( Input.Pressed( "SelectBrush" ) ) BrushAction();
		else if ( Input.Pressed( "SelectFace" ) ) FaceAction();
		else if ( Input.Pressed( "SelectVertex" ) ) VertexAction();
		else if ( Input.Pressed( "SelectEntity" ) ) EntityAction();

		// UNION and INTERSECTION are disabled: the exported CSG format only
		// supports convex brushes and those operators can produce concave
		// results. SUBTRACTION stays — it carves each selected brush out of
		// every other brush whose bounds it overlaps.
		if ( Input.Pressed( "OpSubtract" ) ) SubtractionAction();
		// else if ( Input.Pressed( "OpUnion" ) ) UnionAction();
		// else if ( Input.Pressed( "OpIntersect" ) ) IntersectionAction();

		if ( Input.Pressed( "SelectTexture" ) ) SelectTextureAction();
		if ( Input.Pressed( "DeleteSelection" ) ) DeleteAction();

		if ( Input.Pressed( "HideSelection" ) ) HideAction();
		else if ( Input.Pressed( "UnhideAll" ) ) UnhideAllAction();

		if ( Input.Pressed( "TogglePickTexture" ) ) TogglePickTextureAction();
		if ( Input.Pressed( "RealignTexture" ) ) RealignTextureAction();
		if ( Input.Pressed( "ToggleLockUVs" ) ) ToggleLockUVsAction();

		if ( Input.Pressed( "ToggleLocalGizmo" ) ) ToggleLocalGizmoAction();
		if ( Input.Pressed( "ToggleGrid" ) ) ToggleGridAction();
		if ( Input.Pressed( "ToggleVertexSnap" ) ) ToggleVertexSnapAction();

		if ( Input.Pressed( "DecreaseGrid" ) ) DecreaseGridAction();
		else if ( Input.Pressed( "IncreaseGrid" ) ) IncreaseGridAction();
	}

	public void ToggleLockUVsAction()
	{
		if ( IsPlayMode ) return;
		LockUVs = !LockUVs;
	}

	public void ToggleVertexSnapAction()
	{
		if ( IsPlayMode ) return;
		// Match the toolbar gate: vertex snap is only meaningful while
		// the user is selecting vertices.
		if ( Selection != SelectionMode.Vertex ) return;
		VertexSnap = !VertexSnap;
	}

	public void ToggleMergedSelectionAction()
	{
		if ( IsPlayMode ) return;
		if ( Selection != SelectionMode.Vertex ) return;
		MergedSelection = !MergedSelection;
	}

	public void ToggleSelectionMaskAction()
	{
		if ( IsPlayMode ) return;
		SelectionMask = !SelectionMask;
	}

	private bool IsTextEntryFocused()
	{
		var anchor = _toolbar?.LeftPanelRef ?? _toolbar?.RightPanelRef;
		var root = anchor?.FindRootPanel();
		if ( root is null ) return false;
		return ContainsFocusedTextEntry( root );
	}

	private static bool ContainsFocusedTextEntry( Sandbox.UI.Panel panel )
	{
		if ( panel is Sandbox.UI.TextEntry te && te.HasFocus ) return true;
		foreach ( var child in panel.Children )
		{
			if ( ContainsFocusedTextEntry( child ) ) return true;
		}
		return false;
	}

	private void BlurFocusedTextEntriesOnOutsideClick()
	{
		if ( !Input.Pressed( "attack1" ) ) return;
		var anchor = _toolbar?.LeftPanelRef ?? _toolbar?.RightPanelRef;
		var root = anchor?.FindRootPanel();
		if ( root is null ) return;
		var mouse = Mouse.Position;
		BlurOutside( root, mouse );
	}

	private static void BlurOutside( Sandbox.UI.Panel panel, Vector2 mouse )
	{
		if ( panel is Sandbox.UI.TextEntry te && te.HasFocus && !te.Box.Rect.IsInside( mouse ) )
		{
			te.Blur();
		}
		foreach ( var child in panel.Children )
		{
			BlurOutside( child, mouse );
		}
	}

	private void PruneInvalidSelections()
	{
		_selectedBrushes.RemoveWhere( b => !b.IsValid() );
		_selectedFaces.RemoveWhere( t => !t.Item1.IsValid() );
		_selectedEdges.RemoveWhere( t => !t.Item1.IsValid() );
		_selectedVertices.RemoveAll( t => !t.Item1.IsValid() );
		_selectedCloud.RemoveWhere( c => !c.IsValid() );
		_vertexContextBrushes.RemoveWhere( b => !b.IsValid() );
		_hiddenBrushes.RemoveWhere( b => !b.IsValid() );

		PruneInvalidKeys( _gizmoDragStartPositions );
		PruneInvalidKeys( _rotDragStartTransforms );
		PruneInvalidKeys( _scaleDragStartTransforms );
		PruneInvalidKeys( _gizmoDragStartPositionsCloud );
		PruneInvalidKeys( _rotDragStartTransformsCloud );
		PruneInvalidKeys( _scaleDragStartTransformsCloud );

		List<(ModelBrush, int)> deadVerts = null;
		foreach ( var key in _vertexDragStartLocal.Keys )
		{
			if ( key.brush is null || !key.brush.IsValid() )
			{
				deadVerts ??= new List<(ModelBrush, int)>();
				deadVerts.Add( key );
			}
		}
		if ( deadVerts is not null )
		{
			foreach ( var k in deadVerts ) _vertexDragStartLocal.Remove( k );
		}
	}

	private static void PruneInvalidKeys<TKey, TValue>( Dictionary<TKey, TValue> map ) where TKey : Component
	{
		List<TKey> dead = null;
		foreach ( var key in map.Keys )
		{
			if ( !key.IsValid() )
			{
				dead ??= new List<TKey>();
				dead.Add( key );
			}
		}
		if ( dead is null ) return;
		foreach ( var k in dead ) map.Remove( k );
	}

	private static void ApplySelect<T>( HashSet<T> set, T item, SelectMode mode )
	{
		switch ( mode )
		{
			case SelectMode.Replace:
				set.Clear();
				set.Add( item );
				break;
			case SelectMode.Add:
				if ( !set.Add( item ) ) set.Remove( item );
				break;
			case SelectMode.Remove:
				set.Remove( item );
				break;
		}
	}

	// List-typed variant for ordered selection collections. Preserves the
	// insertion order on Replace / Add while keeping the same toggle semantics
	// as the HashSet form (Add of an already-present item removes it).
	private static void ApplySelectOrdered<T>( List<T> list, T item, SelectMode mode )
	{
		switch ( mode )
		{
			case SelectMode.Replace:
				list.Clear();
				list.Add( item );
				break;
			case SelectMode.Add:
				if ( !list.Remove( item ) ) list.Add( item );
				break;
			case SelectMode.Remove:
				list.Remove( item );
				break;
		}
	}

	private void SelectedAddFace( ModelBrush brush, int hitTriangle, SelectMode mode )
	{
		ApplySelect( _selectedFaces, (brush, hitTriangle), mode );
		RefreshSelectedTexturePath();
	}

	private void SelectedAddVertex( ModelBrush brush, int vertexIndex, SelectMode mode )
	{
		var key = (brush, vertexIndex);
		var wasSelected = _selectedVertices.Contains( key );
		ApplySelectOrdered( _selectedVertices, key, mode );

		if ( !MergedSelection ) return;

		// Expand the action to every vertex in the vertex-context brushes whose
		// world position sits within _mergedSelectionRadius of the picked one,
		// so a shared corner welded across adjacent brushes acts as a single
		// pick. The picked vertex is whatever ApplySelectOrdered just put in
		// the list — direction (add vs remove) follows the picked vertex's
		// own state transition.
		var nowSelected = _selectedVertices.Contains( key );
		var addSiblings = mode == SelectMode.Replace
			|| (mode == SelectMode.Add && nowSelected && !wasSelected);
		var removeSiblings = mode == SelectMode.Remove
			|| (mode == SelectMode.Add && wasSelected && !nowSelected);
		if ( !addSiblings && !removeSiblings ) return;

		foreach ( var (sibBrush, sibIdx) in FindCoincidentVertices( brush, vertexIndex ) )
		{
			var sibKey = (sibBrush, sibIdx);
			if ( addSiblings )
			{
				if ( !_selectedVertices.Contains( sibKey ) ) _selectedVertices.Add( sibKey );
			}
			else
			{
				_selectedVertices.Remove( sibKey );
			}
		}
	}

	private IEnumerable<(ModelBrush brush, int vertexIndex)> FindCoincidentVertices( ModelBrush sourceBrush, int sourceVertexIndex )
	{
		if ( sourceBrush is null || !sourceBrush.IsValid() ) yield break;
		if ( !sourceBrush.TryGetVertexWorldPosition( sourceVertexIndex, out var sourceWorld ) ) yield break;

		var rSq = _mergedSelectionRadius * _mergedSelectionRadius;
		foreach ( var brush in _vertexContextBrushes )
		{
			if ( brush is null || !brush.IsValid() || brush.PolygonMesh is null ) continue;
			var mesh = brush.PolygonMesh;
			foreach ( var v in mesh.VertexHandles )
			{
				if ( brush == sourceBrush && v.Index == sourceVertexIndex ) continue;
				var wp = brush.WorldTransform.PointToWorld( mesh.GetVertexPosition( v ) );
				if ( (wp - sourceWorld).LengthSquared > rSq ) continue;
				yield return (brush, v.Index);
			}
		}
	}

	private void SelectedAddEdge( ModelBrush brush, int hitTriangle, uint edgeIndex, SelectMode mode ) =>
		ApplySelect( _selectedEdges, (brush, hitTriangle, edgeIndex), mode );

	private void SelectedAddBrush( ModelBrush brush, SelectMode mode ) =>
		ApplySelect( _selectedBrushes, brush, mode );

	private void SelectedAddCloud( CloudInstance instance, SelectMode mode ) =>
		ApplySelect( _selectedCloud, instance, mode );

	private static CloudInstance FindCloudInstance( GameObject go )
	{
		while ( go is not null && go.IsValid() )
		{
			var ci = go.GetComponent<CloudInstance>();
			if ( ci is not null ) return ci;
			go = go.Parent;
		}
		return null;
	}

	private IEnumerable<ModelBrush> CollectSelectedBrushes()
	{
		switch ( Selection )
		{
			case SelectionMode.Brush:
				return _selectedBrushes;
			case SelectionMode.Face:
				return _selectedFaces.Select( x => x.Item1 ).Distinct();
			case SelectionMode.Edge:
				return _selectedEdges.Select( x => x.Item1 ).Distinct();
			case SelectionMode.Vertex:
				return _selectedVertices.Select( x => x.Item1 ).Where( b => b is not null ).Distinct();
			case SelectionMode.Entity:
			default:
				return System.Array.Empty<ModelBrush>();
		}
	}

	#endregion

	#region BoxSelect

	// Move-tool marquee. Returns true once the cursor has travelled far
	// enough that we're actively box-selecting (so the dispatch can skip
	// the per-frame hover trace and not paint hover outlines over the
	// rubber-band). Before that threshold, a click-and-release falls back
	// to the regular click-select path.
	private bool UpdateBoxSelect()
	{
		if ( Input.Pressed( "attack1" ) )
		{
			_boxSelectStartScreen = Mouse.Position;
			_boxSelectCurrentScreen = Mouse.Position;
			_boxSelectArmed = true;
			_boxSelecting = false;
			// Defer to HandleSelectionTrace on the press frame so a plain
			// click still hits a brush / face / vertex; the marquee only
			// engages once the cursor has actually moved.
			return false;
		}

		if ( !_boxSelectArmed ) return false;

		if ( Input.Down( "attack1" ) )
		{
			_boxSelectCurrentScreen = Mouse.Position;
			if ( !_boxSelecting
				 && Vector2.Distance( _boxSelectStartScreen, _boxSelectCurrentScreen ) > _boxSelectMinDrag )
			{
				_boxSelecting = true;
			}
			// The visual marquee is drawn as a 2D screen-space rect by the
			// Toolbar overlay panel (which reads IsBoxSelecting / BoxSelect
			// Min / Max). The frustum we build here is only used for the
			// selection test on release.
			return _boxSelecting;
		}

		// Released.
		var committed = _boxSelecting;
		if ( _boxSelecting )
		{
			CommitBoxSelect();
		}
		_boxSelectArmed = false;
		_boxSelecting = false;
		return committed;
	}

	// 8 world-space corners of the marquee's selection frustum: 4 near +
	// 4 far, walked clockwise from the top-left. Stays consistent across
	// the draw and commit paths so the visualisation matches the test.
	private void GetBoxSelectCorners( out Vector3[] near, out Vector3[] far )
	{
		var s1 = _boxSelectStartScreen;
		var s2 = _boxSelectCurrentScreen;
		var minX = MathF.Min( s1.x, s2.x );
		var maxX = MathF.Max( s1.x, s2.x );
		var minY = MathF.Min( s1.y, s2.y );
		var maxY = MathF.Max( s1.y, s2.y );
		var screen = new[]
		{
			new Vector2( minX, minY ),
			new Vector2( maxX, minY ),
			new Vector2( maxX, maxY ),
			new Vector2( minX, maxY ),
		};
		near = new Vector3[4];
		far = new Vector3[4];
		var cam = Scene.Camera;
		for ( var i = 0; i < 4; i++ )
		{
			var ray = cam.ScreenPixelToRay( screen[i] );
			near[i] = ray.Position + ray.Forward * _boxSelectNearDepth;
			far[i] = ray.Position + ray.Forward * _boxSelectFarDepth;
		}
	}

	// Surfaced for the Toolbar overlay so it can draw the rubber-band
	// rectangle as a 2D panel. The Toolbar samples the press point in its
	// own UI coordinate space so the rect tracks the cursor regardless of
	// ScreenPanel's "Auto Screen Scale" — the only thing it needs from us
	// is whether to show the marquee at all.
	public bool IsBoxSelecting => _boxSelecting;

	// Four inward-facing side planes derived from the camera position and
	// the 4 near-corner rays. We skip explicit near/far planes since the
	// marquee is meant to extend "infinitely" along the view direction.
	private readonly Plane[] _boxSelectPlanesScratch = new Plane[4];
	private Plane[] BuildBoxSelectFrustum()
	{
		GetBoxSelectCorners( out var near, out _ );
		var cam = Scene.Camera.WorldPosition;
		// Reference points used to orient each plane's normal inward.
		// For the left plane (camera + TL + BL), the right-edge corners
		// (TR / BR) lie inside the frustum; symmetric for the others.
		_boxSelectPlanesScratch[0] = MakeInwardPlane( cam, near[0], near[3], near[1] ); // left
		_boxSelectPlanesScratch[1] = MakeInwardPlane( cam, near[1], near[2], near[0] ); // right
		_boxSelectPlanesScratch[2] = MakeInwardPlane( cam, near[0], near[1], near[3] ); // top
		_boxSelectPlanesScratch[3] = MakeInwardPlane( cam, near[3], near[2], near[0] ); // bottom
		return _boxSelectPlanesScratch;
	}

	private static Plane MakeInwardPlane( Vector3 origin, Vector3 a, Vector3 b, Vector3 referenceInside )
	{
		var n = Vector3.Cross( a - origin, b - origin ).Normal;
		var plane = new Plane( origin, n );
		if ( plane.GetDistance( referenceInside ) < 0f )
		{
			plane = new Plane( origin, -n );
		}
		return plane;
	}

	private static bool FrustumContainsPoint( Plane[] planes, Vector3 p )
	{
		foreach ( var plane in planes )
		{
			if ( plane.GetDistance( p ) < 0f ) return false;
		}
		return true;
	}

	// Frustum-vs-AABB intersection test. Conservative: only rejects when an
	// entire side plane has every box corner outside, which can produce
	// occasional false positives for boxes that straddle an edge of the
	// frustum. Good enough for marquee selection — false positives just
	// include a brush the user can de-select after the fact.
	private static bool FrustumIntersectsBBox( Plane[] planes, BBox box )
	{
		foreach ( var plane in planes )
		{
			var outside = 0;
			for ( var i = 0; i < 8; i++ )
			{
				var corner = new Vector3(
					(i & 1) != 0 ? box.Maxs.x : box.Mins.x,
					(i & 2) != 0 ? box.Maxs.y : box.Mins.y,
					(i & 4) != 0 ? box.Maxs.z : box.Mins.z );
				if ( plane.GetDistance( corner ) < 0f ) outside++;
			}
			if ( outside == 8 ) return false;
		}
		return true;
	}

	private void CommitBoxSelect()
	{
		var planes = BuildBoxSelectFrustum();
		var mode = CurrentSelectMode();
		switch ( Selection )
		{
			case SelectionMode.Brush:
				CommitBoxSelectBrushes( planes, mode );
				break;
			case SelectionMode.Face:
				CommitBoxSelectFaces( planes, mode );
				break;
			case SelectionMode.Vertex:
				CommitBoxSelectVertices( planes, mode );
				break;
			case SelectionMode.Entity:
				CommitBoxSelectEntities( planes, mode );
				break;
		}
	}

	private void CommitBoxSelectBrushes( Plane[] planes, SelectMode mode )
	{
		var hits = new List<ModelBrush>();
		foreach ( var brush in ModelBrush.Brushes )
		{
			if ( brush is null || !brush.IsValid() ) continue;
			if ( _hiddenBrushes.Contains( brush ) ) continue;
			ComputeWorldAABB( brush.Bounds, brush.WorldTransform, out var box );
			if ( FrustumIntersectsBBox( planes, box ) ) hits.Add( brush );
		}
		ApplyBatchSelection( _selectedBrushes, hits, mode );
	}

	private void CommitBoxSelectFaces( Plane[] planes, SelectMode mode )
	{
		// Face mode only allows picking faces from brushes that already
		// have at least one face selected, mirroring the vertex-mode
		// context restriction. Without an active set there's nothing to
		// box-select.
		var context = new HashSet<ModelBrush>();
		foreach ( var (brush, _) in _selectedFaces )
		{
			if ( brush is null || !brush.IsValid() ) continue;
			context.Add( brush );
		}
		if ( context.Count == 0 ) return;

		var hits = new List<(ModelBrush, int)>();
		foreach ( var brush in context )
		{
			var mesh = brush.PolygonMesh;
			if ( mesh is null ) continue;
			var xform = brush.WorldTransform;
			var seen = new HashSet<int>();
			var indices = brush.Model?.GetIndices();
			if ( indices is null ) continue;
			var triCount = indices.Length / 3;
			for ( var t = 0; t < triCount; t++ )
			{
				var face = mesh.TriangleToFace( t );
				if ( !seen.Add( face.Index ) ) continue;
				if ( !FaceInsideFrustum( mesh, face, xform, planes ) ) continue;
				hits.Add( (brush, t) );
			}
		}
		ApplyBatchSelection( _selectedFaces, hits, mode );
		RefreshSelectedTexturePath();
	}

	private static bool FaceInsideFrustum( PolygonMesh mesh, FaceHandle face, Transform xform, Plane[] planes )
	{
		var verts = mesh.GetFaceVertices( face );
		if ( verts is null || verts.Length == 0 ) return false;
		foreach ( var v in verts )
		{
			var world = xform.PointToWorld( mesh.GetVertexPosition( v ) );
			if ( !FrustumContainsPoint( planes, world ) ) return false;
		}
		return true;
	}

	private void CommitBoxSelectVertices( Plane[] planes, SelectMode mode )
	{
		if ( _vertexContextBrushes.Count == 0 ) return;
		var hits = new List<(ModelBrush, int)>();
		foreach ( var brush in _vertexContextBrushes )
		{
			if ( brush is null || !brush.IsValid() ) continue;
			var mesh = brush.PolygonMesh;
			if ( mesh is null ) continue;
			var xform = brush.WorldTransform;
			foreach ( var handle in mesh.VertexHandles )
			{
				var world = xform.PointToWorld( mesh.GetVertexPosition( handle ) );
				if ( FrustumContainsPoint( planes, world ) ) hits.Add( (brush, handle.Index) );
			}
		}
		ApplyBatchSelectionOrdered( _selectedVertices, hits, mode );
	}

	private void CommitBoxSelectEntities( Plane[] planes, SelectMode mode )
	{
		var hits = new List<CloudInstance>();
		foreach ( var ci in CloudInstance.All )
		{
			if ( ci is null || !ci.IsValid() ) continue;
			if ( ci.GameObject is null || !ci.GameObject.Enabled ) continue;
			if ( !TryGetCloudLocalBounds( ci, out var local ) )
			{
				if ( FrustumContainsPoint( planes, ci.WorldPosition ) ) hits.Add( ci );
				continue;
			}
			ComputeWorldAABB( local, ci.WorldTransform, out var world );
			if ( FrustumIntersectsBBox( planes, world ) ) hits.Add( ci );
		}
		ApplyBatchSelection( _selectedCloud, hits, mode );
	}

	private static void ComputeWorldAABB( BBox local, Transform transform, out BBox world )
	{
		var min = new Vector3( float.PositiveInfinity, float.PositiveInfinity, float.PositiveInfinity );
		var max = new Vector3( float.NegativeInfinity, float.NegativeInfinity, float.NegativeInfinity );
		ExpandWorldBounds( local, transform, ref min, ref max );
		world = new BBox( min, max );
	}

	// Replace / Add / Remove semantics for a HashSet against a collected
	// batch. Add never toggles (unlike the single-pick ApplySelect path):
	// for a marquee the user expects items inside the box to be added,
	// not flipped if they were already selected.
	private static void ApplyBatchSelection<T>( HashSet<T> set, IReadOnlyCollection<T> batch, SelectMode mode )
	{
		switch ( mode )
		{
			case SelectMode.Replace:
				set.Clear();
				foreach ( var item in batch ) set.Add( item );
				break;
			case SelectMode.Add:
				foreach ( var item in batch ) set.Add( item );
				break;
			case SelectMode.Remove:
				foreach ( var item in batch ) set.Remove( item );
				break;
		}
	}

	private static void ApplyBatchSelectionOrdered<T>( List<T> list, IReadOnlyCollection<T> batch, SelectMode mode )
	{
		switch ( mode )
		{
			case SelectMode.Replace:
				list.Clear();
				foreach ( var item in batch ) if ( !list.Contains( item ) ) list.Add( item );
				break;
			case SelectMode.Add:
				foreach ( var item in batch ) if ( !list.Contains( item ) ) list.Add( item );
				break;
			case SelectMode.Remove:
				foreach ( var item in batch ) list.Remove( item );
				break;
		}
	}

	#endregion

	#region Interface

	private void ToggleInterface()
	{
		_hidden = !_hidden;
		Mouse.Visibility = _hidden
			? MouseVisibility.Visible
			: MouseVisibility.Hidden;
		_toolbar.LeftPanelRef.Style.Display = _hidden ? DisplayMode.None : DisplayMode.Flex;
		_toolbar.RightPanelRef.Style.Display = _hidden ? DisplayMode.None : DisplayMode.Flex;
		_scalingUp = false;
		_gizmoActiveAxis = GizmoAxis.None;
		_gizmoHoveredAxis = GizmoAxis.None;
		_gizmoDragStartPositions.Clear();
		_gizmoDragStartPositionsCloud.Clear();
		_vertexDragStartLocal.Clear();
		_vertexDragAnchor = null;
		_gizmoShiftPendingClone = false;
		_rotActiveAxis = GizmoAxis.None;
		_rotHoveredAxis = GizmoAxis.None;
		_rotDragStartTransforms.Clear();
		_rotDragStartTransformsCloud.Clear();
		_rotDragBasisOverride = null;
		_scaleActiveSide = GizmoAxisSide.None;
		_scaleHoveredSide = GizmoAxisSide.None;
		_scaleDragStartTransforms.Clear();
		_scaleDragStartTransformsCloud.Clear();
		_sliceDragging = false;
		_sliceHasPick = false;
		_boxSelectArmed = false;
		_boxSelecting = false;
	}

	#endregion

	#region SelectionGizmo

	private Vector3 GizmoAxisDir( GizmoAxis axis )
	{
		var basis = GizmoBasis();
		return axis switch
		{
			GizmoAxis.X => (Vector3.Forward * basis).Normal,
			GizmoAxis.Y => (Vector3.Left * basis).Normal,
			GizmoAxis.Z => (Vector3.Up * basis).Normal,
			_ => Vector3.Zero,
		};
	}

	private static bool IsDiagonalAxis( GizmoAxis a ) =>
		a == GizmoAxis.XY || a == GizmoAxis.YZ || a == GizmoAxis.XZ;

	// The two main axes that span a diagonal handle's plane.
	private static (GizmoAxis a, GizmoAxis b) DiagonalComponentAxes( GizmoAxis axis ) => axis switch
	{
		GizmoAxis.XY => (GizmoAxis.X, GizmoAxis.Y),
		GizmoAxis.YZ => (GizmoAxis.Y, GizmoAxis.Z),
		GizmoAxis.XZ => (GizmoAxis.X, GizmoAxis.Z),
		_ => (GizmoAxis.None, GizmoAxis.None),
	};

	// Plane normal for a diagonal handle = the remaining main axis (the one NOT
	// in its component pair). Drags on the diagonal are constrained to this
	// plane, so the third axis stays put.
	private Vector3 DiagonalPlaneNormal( GizmoAxis axis ) => axis switch
	{
		GizmoAxis.XY => GizmoAxisDir( GizmoAxis.Z ),
		GizmoAxis.YZ => GizmoAxisDir( GizmoAxis.X ),
		GizmoAxis.XZ => GizmoAxisDir( GizmoAxis.Y ),
		_ => Vector3.Zero,
	};

	// World basis (identity) by default. When LocalGizmo is on and a single
	// brush or single entity is selected, use that shape's world rotation so
	// the gizmo arrows and rings line up with its local axes. Multi-select
	// has no shared local frame, so we fall back to world. Vertex mode also
	// stays in world: vertex translation snaps each vertex against the world
	// grid, which would distort along a tilted axis.
	private Rotation GizmoBasis()
	{
		if ( _rotDragBasisOverride.HasValue ) return _rotDragBasisOverride.Value;
		if ( !LocalGizmo ) return Rotation.Identity;
		if ( Selection == SelectionMode.Brush && _selectedBrushes.Count == 1 )
		{
			var brush = _selectedBrushes.First();
			if ( brush.IsValid() ) return brush.WorldRotation;
		}
		else if ( Selection == SelectionMode.Entity && _selectedCloud.Count == 1 )
		{
			var ci = _selectedCloud.First();
			if ( ci.IsValid() ) return ci.WorldRotation;
		}
		return Rotation.Identity;
	}

	private bool IsTransformableSelection() =>
		Selection == SelectionMode.Brush || Selection == SelectionMode.Entity;

	// Rotate operates on whole shapes only; Move and Scale additionally
	// drive vertex selections — Move on any number of vertices, Scale on
	// two or more (a single vertex has no bounds for a scale frame).
	private bool IsMoveTransformable() =>
		IsTransformableSelection()
		|| (Selection == SelectionMode.Vertex && _selectedVertices.Count > 0);

	private bool IsScaleTransformable() =>
		IsTransformableSelection()
		|| (Selection == SelectionMode.Vertex && _selectedVertices.Count >= 2);

	private bool TryGetSelectionCenter( out Vector3 center )
	{
		center = Vector3.Zero;
		var count = 0;

		switch ( Selection )
		{
			case SelectionMode.Brush:
				foreach ( var brush in _selectedBrushes )
				{
					center += brush.WorldTransform.PointToWorld( brush.Bounds.Center );
					count++;
				}
				break;
			case SelectionMode.Entity:
				foreach ( var ci in _selectedCloud )
				{
					if ( !ci.IsValid() ) continue;
					center += ci.WorldPosition;
					count++;
				}
				break;
			case SelectionMode.Face:
				foreach ( var (brush, _) in _selectedFaces )
				{
					center += brush.WorldTransform.PointToWorld( brush.Bounds.Center );
					count++;
				}
				break;
			case SelectionMode.Edge:
				foreach ( var (brush, _, _) in _selectedEdges )
				{
					center += brush.WorldTransform.PointToWorld( brush.Bounds.Center );
					count++;
				}
				break;
			case SelectionMode.Vertex:
				foreach ( var (brush, vIdx) in _selectedVertices )
				{
					if ( brush is null || !brush.IsValid() ) continue;
					if ( !brush.TryGetVertexWorldPosition( vIdx, out var world ) ) continue;
					center += world;
					count++;
				}
				break;
		}

		if ( count == 0 ) return false;
		center /= count;
		return true;
	}

	private void UpdateGizmo()
	{
		if ( !IsMoveTransformable() || CurrentTool != Tool.Move )
		{
			_gizmoVisible = false;
			_gizmoHoveredAxis = GizmoAxis.None;
			return;
		}

		Vector3 center;
		var alignToGrid = true;
		if ( Selection == SelectionMode.Vertex )
		{
			// In Vertex mode the gizmo sits on a single selected vertex —
			// whichever one is closest to the cursor ray while hovering, and
			// the drag anchor once a drag has started (the anchor is the
			// only vertex that gets snapped; the rest follow rigidly). This
			// makes it obvious which vertex you're about to snap before you
			// even click. Skip AlignToGrid: the gizmo is meant to sit
			// exactly on the vertex it represents, even when the vertex is
			// off-grid mid-edit.
			if ( !TryGetVertexGizmoCenter( out center ) )
			{
				_gizmoVisible = false;
				_gizmoHoveredAxis = GizmoAxis.None;
				return;
			}
			alignToGrid = false;
		}
		else if ( !TryGetSelectionCenter( out center ) )
		{
			_gizmoVisible = false;
			_gizmoHoveredAxis = GizmoAxis.None;
			return;
		}

		_gizmoVisible = true;
		// Snap the gizmo origin onto the grid. The rotate gizmo captures
		// _rotGizmoCenter as its drag pivot, and rotating around an off-grid
		// pivot pulls grid-aligned brushes off the grid (the centroid of an
		// odd-sized or multi-brush selection is rarely on a grid point).
		// Snapping the move gizmo here too keeps both tools visually
		// consistent. AlignToGrid is a no-op when the grid is disabled.
		if ( alignToGrid ) AlignToGrid( ref center );
		_gizmoCenter = center;

		var distance = Vector3.DistanceBetween( Scene.Camera.WorldPosition, _gizmoCenter );
		_gizmoScale = MathF.Max( _gizmoMinScale, distance / _gizmoReferenceDistance );

		DrawGizmoArrow( GizmoAxis.X, _gizmoXColor );
		DrawGizmoArrow( GizmoAxis.Y, _gizmoYColor );
		DrawGizmoArrow( GizmoAxis.Z, _gizmoZColor );
		DrawGizmoDiagonal( GizmoAxis.XY, _gizmoXYColor );
		DrawGizmoDiagonal( GizmoAxis.YZ, _gizmoYZColor );
		DrawGizmoDiagonal( GizmoAxis.XZ, _gizmoXZColor );
	}

	// Vertex-mode gizmo origin. Sits on the first-selected vertex (the snap
	// anchor on multi-vertex drags); during a drag it stays pinned at that
	// vertex's drag-start position so the drag math (axis projections / plane
	// intersections) is anchored to a stable origin even if the anchor's snap
	// pulls it off-axis.
	private bool TryGetVertexGizmoCenter( out Vector3 center )
	{
		if ( _gizmoActiveAxis != GizmoAxis.None )
		{
			center = _gizmoCenter;
			return true;
		}

		center = Vector3.Zero;
		foreach ( var (brush, vIdx) in _selectedVertices )
		{
			if ( brush is null || !brush.IsValid() ) continue;
			if ( !brush.TryGetVertexWorldPosition( vIdx, out center ) ) continue;
			return true;
		}
		return false;
	}

	private void DrawGizmoDiagonal( GizmoAxis axis, Color baseColor )
	{
		var (axA, axB) = DiagonalComponentAxes( axis );
		var mid = ScaledDiagonalMid;
		var endA = _gizmoCenter + GizmoAxisDir( axA ) * mid;
		var endB = _gizmoCenter + GizmoAxisDir( axB ) * mid;
		var color = (axis == _gizmoHoveredAxis || axis == _gizmoActiveAxis) ? _gizmoHighlightColor : baseColor;
		CustomOverlay.Line( endA, endB, color, 0f, default, true, GizmoLineThickness );
	}

	private void DrawGizmoArrow( GizmoAxis axis, Color baseColor )
	{
		var dir = GizmoAxisDir( axis );
		var origin = _gizmoCenter;
		var arrowLength = ScaledArrowLength;
		var headSize = ScaledHeadSize;
		var tip = origin + dir * arrowLength;
		var color = (axis == _gizmoHoveredAxis || axis == _gizmoActiveAxis) ? _gizmoHighlightColor : baseColor;

		CustomOverlay.Line( origin, tip, color, 0f, default, true, GizmoLineThickness );

		var perp1 = Vector3.Cross( dir, MathF.Abs( dir.z ) > _crossHelperZThreshold ? Vector3.Right : Vector3.Up ).Normal;
		var perp2 = Vector3.Cross( dir, perp1 ).Normal;
		var basePoint = tip - dir * headSize;
		var headHalf = headSize * _gizmoHeadHalfRatio;
		CustomOverlay.Line( tip, basePoint + perp1 * headHalf, color, 0f, default, true, GizmoLineThickness );
		CustomOverlay.Line( tip, basePoint - perp1 * headHalf, color, 0f, default, true, GizmoLineThickness );
		CustomOverlay.Line( tip, basePoint + perp2 * headHalf, color, 0f, default, true, GizmoLineThickness );
		CustomOverlay.Line( tip, basePoint - perp2 * headHalf, color, 0f, default, true, GizmoLineThickness );
	}

	private bool UpdateGizmoDrag( Ray cameraRay )
	{
		if ( !_gizmoVisible ) return false;

		var isVertexDrag = Selection == SelectionMode.Vertex;

		if ( _gizmoActiveAxis != GizmoAxis.None )
		{
			if ( !Input.Down( "attack1" ) )
			{
				var publishKeys = isVertexDrag
					? _vertexDragStartLocal.Keys.Select( k => k.brush ).Distinct().ToList()
					: _gizmoDragStartPositions.Keys.ToList();
				PublishUVSnapshotToPeers( publishKeys );
				_gizmoActiveAxis = GizmoAxis.None;
				_gizmoDragStartPositions.Clear();
				_gizmoDragStartPositionsCloud.Clear();
				_vertexDragStartLocal.Clear();
				_vertexDragAnchor = null;
				_gizmoShiftPendingClone = false;
				_undo.EndEdit();
				return false;
			}

			if ( TryGetDragTranslation( cameraRay, out var translation ) )
			{
				if ( isVertexDrag )
				{
					ApplyVertexTranslation( translation );
				}
				else
				{
					// Snap onto the grid in axis-space. For a single main axis,
					// project the translation onto its direction and round
					// (avoids tilted-LocalGizmo axes drifting off when each
					// world component is rounded independently). For a diagonal
					// plane handle, project onto each of the two component
					// axes and round each separately so both axes land on grid
					// lines simultaneously.
					if ( _gridEnabled && _gridSize > 0f )
					{
						translation = SnapDragTranslation( translation );
					}

					if ( _gizmoShiftPendingClone && translation.LengthSquared > 0f )
					{
						CloneSelectionInPlace();
						_gizmoDragStartPositions.Clear();
						foreach ( var brush in CollectSelectedBrushes() )
						{
							_gizmoDragStartPositions[brush] = brush.WorldPosition;
						}
						_gizmoDragStartPositionsCloud.Clear();
						foreach ( var ci in _selectedCloud )
						{
							if ( !ci.IsValid() ) continue;
							_gizmoDragStartPositionsCloud[ci] = ci.WorldPosition;
						}
						_gizmoShiftPendingClone = false;
					}

					foreach ( var (brush, startPos) in _gizmoDragStartPositions )
					{
						if ( !brush.IsValid() ) continue;
						brush.WorldPosition = startPos + translation;
					}
					foreach ( var (ci, startPos) in _gizmoDragStartPositionsCloud )
					{
						if ( !ci.IsValid() ) continue;
						ci.WorldPosition = startPos + translation;
					}
				}
			}
			return true;
		}

		if ( !TraceGizmo( cameraRay, out var hovered ) )
		{
			_gizmoHoveredAxis = GizmoAxis.None;
			return false;
		}
		_gizmoHoveredAxis = hovered;

		if ( Input.Pressed( "attack1" ) )
		{
			_gizmoActiveAxis = hovered;
			_gizmoDragAxisDir = GizmoAxisDir( hovered );
			if ( IsDiagonalAxis( hovered ) )
			{
				// Diagonal handles drag in a plane — capture the ray-plane
				// intersection at the gizmo centre so deltas come out as full
				// 2D plane translations rather than projections onto a single
				// axis.
				IntersectRayWithPlane( cameraRay, _gizmoCenter, DiagonalPlaneNormal( hovered ), out _gizmoDragStart );
			}
			else
			{
				ClosestPointOnAxis( cameraRay, _gizmoCenter, _gizmoDragAxisDir, out _gizmoDragStart );
			}

			_gizmoShiftPendingClone = !isVertexDrag && Input.Down( "Run" );

			_gizmoDragStartPositions.Clear();
			_gizmoDragStartPositionsCloud.Clear();
			_vertexDragStartLocal.Clear();

			if ( isVertexDrag )
			{
				var dragBrushes = CollectSelectedBrushes().ToList();
				foreach ( var (brush, vIdx) in _selectedVertices )
				{
					if ( brush is null || !brush.IsValid() ) continue;
					if ( !brush.CanLocalEdit() ) continue;
					if ( !brush.TryGetVertexLocalPosition( vIdx, out var local ) ) continue;
					_vertexDragStartLocal[(brush, vIdx)] = local;
				}
				_vertexDragAnchor = PickVertexAnchor();
				_undo.BeginEdit( dragBrushes );
			}
			else
			{
				var dragBrushes = CollectSelectedBrushes().ToList();
				foreach ( var brush in dragBrushes )
				{
					brush.TakeOwnershipForEdit();
					_gizmoDragStartPositions[brush] = brush.WorldPosition;
				}
				var dragCloud = _selectedCloud.Where( c => c.IsValid() ).ToList();
				foreach ( var ci in dragCloud )
				{
					_gizmoDragStartPositionsCloud[ci] = ci.WorldPosition;
				}
				_undo.BeginEdit( dragBrushes );
				foreach ( var ci in dragCloud )
				{
					_undo.TrackExistingCloud( ci );
				}
			}
		}
		return true;
	}

	// Apply a world-space translation to each tracked selected vertex. Each
	// vertex's desired world position first tries to snap onto another vertex
	// (any brush, current pose) within the configured radius, then falls back
	// to grid alignment. We rebuild the affected brush's polygon mesh once
	// per call and remap (brush, vertexIndex) keys via the returned index
	// table so subsequent frames keep tracking the same vertices.
	private void ApplyVertexTranslation( Vector3 worldTranslation )
	{
		if ( _vertexDragStartLocal.Count == 0 ) return;

		// Snap only the anchor vertex (the one closest to the cursor ray when
		// the drag started). The snap correction it picks up gets applied as a
		// rigid translation to every other selected vertex, so the selection
		// keeps its shape instead of each vertex independently snapping to its
		// nearest neighbour and pulling the group apart.
		var effective = worldTranslation;
		if ( _vertexDragAnchor.HasValue
			 && _vertexDragStartLocal.TryGetValue( _vertexDragAnchor.Value, out var anchorStartLocal ) )
		{
			var (anchorBrush, anchorIdx) = _vertexDragAnchor.Value;
			if ( anchorBrush is not null && anchorBrush.IsValid() && anchorBrush.CanLocalEdit() )
			{
				var anchorStartWorld = anchorBrush.WorldTransform.PointToWorld( anchorStartLocal );
				var anchorDesired = anchorStartWorld + worldTranslation;
				var anchorSnapped = SnapVertexWorldPosition( anchorDesired, anchorBrush, anchorIdx );
				effective = anchorSnapped - anchorStartWorld;
			}
		}

		var byBrush = new Dictionary<ModelBrush, Dictionary<int, Vector3>>();
		foreach ( var ((brush, vIdx), startLocal) in _vertexDragStartLocal )
		{
			if ( brush is null || !brush.IsValid() ) continue;
			if ( !brush.CanLocalEdit() ) continue;

			var startWorld = brush.WorldTransform.PointToWorld( startLocal );
			var newWorld = startWorld + effective;
			var newLocal = brush.WorldTransform.PointToLocal( newWorld );

			if ( !byBrush.TryGetValue( brush, out var moves ) )
			{
				moves = new Dictionary<int, Vector3>();
				byBrush[brush] = moves;
			}
			moves[vIdx] = newLocal;
		}

		foreach ( var (brush, moves) in byBrush )
		{
			var remap = brush.MoveVerticesLocal( moves );
			if ( remap is null || remap.Count == 0 ) continue;
			RemapVertexKeys( brush, remap );
		}
	}

	// First-selected vertex (in _selectedVertices order) that's also part of
	// this drag's starting set. The move gizmo sits on it and it's the only
	// vertex SnapVertexWorldPosition gets applied to — the rest follow rigidly.
	private (ModelBrush brush, int vertexIndex)? PickVertexAnchor()
	{
		foreach ( var (brush, vIdx) in _selectedVertices )
		{
			if ( brush is null || !brush.IsValid() ) continue;
			if ( _vertexDragStartLocal.ContainsKey( (brush, vIdx) ) ) return (brush, vIdx);
		}
		return null;
	}

	private Vector3 SnapVertexWorldPosition( Vector3 worldPos, ModelBrush sourceBrush, int sourceVertexIndex )
	{
		// VertexSnap off: skip the vertex-on-vertex search entirely and fall
		// straight through to grid alignment, matching the user expectation
		// that the "Snap" toggle controls vertex-to-vertex snapping only.
		if ( !VertexSnap )
		{
			AlignToGrid( ref worldPos );
			return worldPos;
		}

		// Snap radius scales with grid spacing so the snap zone feels
		// consistent regardless of the current grid; fall back to a small
		// constant when the grid is disabled.
		var snapRadius = _gridSize > 0f ? _gridSize * 0.5f : _vertexSnapHitRadius;
		var bestDistSq = snapRadius * snapRadius;
		var snapped = worldPos;
		var found = false;

		foreach ( var brush in ModelBrush.Brushes )
		{
			if ( brush is null || !brush.IsValid() || brush.PolygonMesh is null ) continue;
			var mesh = brush.PolygonMesh;
			foreach ( var v in mesh.VertexHandles )
			{
				// Skip every vertex that's also moving this frame so we don't
				// snap a moving vertex to another moving vertex's stale pose.
				if ( _vertexDragStartLocal.ContainsKey( (brush, v.Index) ) ) continue;
				if ( brush == sourceBrush && v.Index == sourceVertexIndex ) continue;
				var wp = brush.WorldTransform.PointToWorld( mesh.GetVertexPosition( v ) );
				var distSq = (wp - worldPos).LengthSquared;
				if ( distSq >= bestDistSq ) continue;
				bestDistSq = distSq;
				snapped = wp;
				found = true;
			}
		}

		if ( found ) return snapped;
		AlignToGrid( ref worldPos );
		return worldPos;
	}

	private void RemapVertexKeys( ModelBrush brush, Dictionary<int, int> remap )
	{
		if ( remap.Count == 0 ) return;
		var anyChange = false;
		foreach ( var (oldIdx, newIdx) in remap )
		{
			if ( oldIdx != newIdx ) { anyChange = true; break; }
		}
		if ( !anyChange ) return;

		var newDrag = new Dictionary<(ModelBrush brush, int vertexIndex), Vector3>( _vertexDragStartLocal.Count );
		foreach ( var (key, val) in _vertexDragStartLocal )
		{
			if ( key.brush == brush && remap.TryGetValue( key.vertexIndex, out var ni ) )
			{
				newDrag[(brush, ni)] = val;
			}
			else
			{
				newDrag[key] = val;
			}
		}
		_vertexDragStartLocal.Clear();
		foreach ( var (k, v) in newDrag ) _vertexDragStartLocal[k] = v;

		// Rebuild the ordered selection list in place: remap each entry,
		// dedupe in case the remap collapses two indices, preserve order so
		// the first-selected vertex stays first (it's the snap anchor).
		var newSel = new List<(ModelBrush, int)>( _selectedVertices.Count );
		foreach ( var (b, vi) in _selectedVertices )
		{
			var entry = (b == brush && remap.TryGetValue( vi, out var ni )) ? (b, ni) : (b, vi);
			if ( !newSel.Contains( entry ) ) newSel.Add( entry );
		}
		_selectedVertices.Clear();
		_selectedVertices.AddRange( newSel );

		if ( _vertexDragAnchor.HasValue
			 && _vertexDragAnchor.Value.brush == brush
			 && remap.TryGetValue( _vertexDragAnchor.Value.vertexIndex, out var newAnchorIdx ) )
		{
			_vertexDragAnchor = (brush, newAnchorIdx);
		}
	}

	private bool TraceGizmo( Ray cameraRay, out GizmoAxis axis )
	{
		axis = GizmoAxis.None;
		var rayDir = cameraRay.Forward;
		var bestDist = float.PositiveInfinity;
		var arrowLength = ScaledArrowLength;
		var mid = ScaledDiagonalMid;
		var hitRadius = ScaledHitRadius;

		for ( var a = GizmoAxis.X; a <= GizmoAxis.Z; a++ )
		{
			var dir = GizmoAxisDir( a );
			var tip = _gizmoCenter + dir * arrowLength;
			if ( !ClosestRayToSegment( cameraRay.Position, rayDir, MaxRayDistance, _gizmoCenter, tip,
				out var rayDistance, out var separation ) )
				continue;
			if ( separation > hitRadius ) continue;
			if ( rayDistance >= bestDist ) continue;
			bestDist = rayDistance;
			axis = a;
		}

		foreach ( var a in _diagonalAxes )
		{
			var (axA, axB) = DiagonalComponentAxes( a );
			var endA = _gizmoCenter + GizmoAxisDir( axA ) * mid;
			var endB = _gizmoCenter + GizmoAxisDir( axB ) * mid;
			if ( !ClosestRayToSegment( cameraRay.Position, rayDir, MaxRayDistance, endA, endB,
				out var rayDistance, out var separation ) )
				continue;
			if ( separation > hitRadius ) continue;
			if ( rayDistance >= bestDist ) continue;
			bestDist = rayDistance;
			axis = a;
		}

		return axis != GizmoAxis.None;
	}

	private static readonly GizmoAxis[] _diagonalAxes = { GizmoAxis.XY, GizmoAxis.YZ, GizmoAxis.XZ };

	// Frame translation from the cursor ray. Main axes project the ray onto
	// the axis line (1D). Diagonal handles intersect the ray with the plane
	// formed by their two component axes (2D — free movement within the
	// plane, third axis stays put).
	private bool TryGetDragTranslation( Ray cameraRay, out Vector3 translation )
	{
		translation = Vector3.Zero;
		if ( IsDiagonalAxis( _gizmoActiveAxis ) )
		{
			if ( !IntersectRayWithPlane( cameraRay, _gizmoCenter, DiagonalPlaneNormal( _gizmoActiveAxis ), out var current ) )
				return false;
			translation = current - _gizmoDragStart;
			return true;
		}
		if ( !ClosestPointOnAxis( cameraRay, _gizmoCenter, _gizmoDragAxisDir, out var p ) )
			return false;
		var delta = (p - _gizmoDragStart).Dot( _gizmoDragAxisDir );
		translation = _gizmoDragAxisDir * delta;
		return true;
	}

	private Vector3 SnapDragTranslation( Vector3 translation )
	{
		if ( IsDiagonalAxis( _gizmoActiveAxis ) )
		{
			var (axA, axB) = DiagonalComponentAxes( _gizmoActiveAxis );
			var dirA = GizmoAxisDir( axA );
			var dirB = GizmoAxisDir( axB );
			var compA = MathF.Round( Vector3.Dot( translation, dirA ) / _gridSize ) * _gridSize;
			var compB = MathF.Round( Vector3.Dot( translation, dirB ) / _gridSize ) * _gridSize;
			return dirA * compA + dirB * compB;
		}
		var delta = Vector3.Dot( translation, _gizmoDragAxisDir );
		var snapped = MathF.Round( delta / _gridSize ) * _gridSize;
		return _gizmoDragAxisDir * snapped;
	}

	private static bool IntersectRayWithPlane( Ray ray, Vector3 planePoint, Vector3 planeNormal, out Vector3 point )
	{
		point = planePoint;
		var denom = Vector3.Dot( ray.Forward, planeNormal );
		if ( MathF.Abs( denom ) < _axisEpsilon ) return false;
		var t = Vector3.Dot( planePoint - ray.Position, planeNormal ) / denom;
		if ( t < 0f || t > MaxRayDistance ) return false;
		point = ray.Position + ray.Forward * t;
		return true;
	}

	private static bool ClosestPointOnAxis( Ray ray, Vector3 origin, Vector3 axisDir, out Vector3 axisPoint )
	{
		axisPoint = origin;
		var rayDir = ray.Forward;
		var w0 = ray.Position - origin;
		var b = Vector3.Dot( rayDir, axisDir );
		var c = Vector3.Dot( axisDir, axisDir );
		var d = Vector3.Dot( rayDir, w0 );
		var e = Vector3.Dot( axisDir, w0 );
		var denom = c - b * b;
		if ( denom < _axisEpsilon ) return false;
		var t = (e - b * d) / denom;
		axisPoint = origin + axisDir * t;
		return true;
	}

	private static bool ClosestRayToSegment( Vector3 rayOrigin, Vector3 rayDir, float maxDist,
		Vector3 a, Vector3 b, out float rayT, out float separation )
	{
		rayT = 0f;
		separation = float.PositiveInfinity;

		var d2 = b - a;
		var seg2 = Vector3.Dot( d2, d2 );
		if ( seg2 <= 0f ) return false;

		var r = rayOrigin - a;
		var dotDirSeg = Vector3.Dot( rayDir, d2 );
		var dotSegR = Vector3.Dot( d2, r );
		var dotDirR = Vector3.Dot( rayDir, r );
		var denom = seg2 - dotDirSeg * dotDirSeg;

		var t = denom > _axisEpsilon ? (dotDirSeg * dotSegR - dotDirR * seg2) / denom : 0f;
		t = Math.Clamp( t, 0f, maxDist );

		var s = Math.Clamp( (dotDirSeg * t + dotSegR) / seg2, 0f, 1f );
		t = Math.Clamp( s * dotDirSeg - dotDirR, 0f, maxDist );

		var rayPoint = rayOrigin + rayDir * t;
		var segPoint = a + d2 * s;
		separation = (rayPoint - segPoint).Length;
		rayT = t;
		return true;
	}

	#endregion

	#region RotateGizmo

	private (Vector3 U, Vector3 V) GizmoRingBasis( GizmoAxis axis )
	{
		var dir = GizmoAxisDir( axis );
		var u = Vector3.Cross( dir, MathF.Abs( dir.z ) > _crossHelperZThreshold ? Vector3.Right : Vector3.Up ).Normal;
		var v = Vector3.Cross( dir, u ).Normal;
		return (u, v);
	}

	private void UpdateRotateGizmo()
	{
		if ( !IsTransformableSelection() || CurrentTool != Tool.Rotate || !TryGetSelectionCenter( out var center ) )
		{
			_rotGizmoVisible = false;
			_rotHoveredAxis = GizmoAxis.None;
			return;
		}
		_rotGizmoVisible = true;
		// Snap the rotate gizmo origin to the grid; _rotDragPivot is captured
		// from this on drag start, so without snapping a multi-brush or
		// odd-bounds selection rotates around an off-grid pivot and the
		// resulting WorldPosition (pivot + offset * rotation) lands off-grid
		// even for 90° increments of grid-aligned starts. No-op when grid
		// is disabled.
		AlignToGrid( ref center );
		_rotGizmoCenter = _rotActiveAxis != GizmoAxis.None ? _rotDragPivot : center;

		var distance = Vector3.DistanceBetween( Scene.Camera.WorldPosition, _rotGizmoCenter );
		_rotGizmoScale = MathF.Max( _gizmoMinScale, distance / _gizmoReferenceDistance );

		DrawGizmoRing( GizmoAxis.X, _gizmoXColor );
		DrawGizmoRing( GizmoAxis.Y, _gizmoYColor );
		DrawGizmoRing( GizmoAxis.Z, _gizmoZColor );
	}

	private void DrawGizmoRing( GizmoAxis axis, Color baseColor )
	{
		var (u, v) = GizmoRingBasis( axis );
		var radius = ScaledRingRadius;
		var color = (axis == _rotHoveredAxis || axis == _rotActiveAxis) ? _gizmoHighlightColor : baseColor;

		var prev = _rotGizmoCenter + u * radius;
		for ( var i = 1; i <= _gizmoRingSegments; i++ )
		{
			var t = i * MathF.PI * 2f / _gizmoRingSegments;
			var p = _rotGizmoCenter + (u * MathF.Cos( t ) + v * MathF.Sin( t )) * radius;
			CustomOverlay.Line( prev, p, color, 0f, default, true, GizmoLineThickness );
			prev = p;
		}
	}

	private bool UpdateRotateGizmoDrag( Ray cameraRay )
	{
		if ( !_rotGizmoVisible ) return false;

		if ( _rotActiveAxis != GizmoAxis.None )
		{
			if ( !Input.Down( "attack1" ) )
			{
				PublishUVSnapshotToPeers( _rotDragStartTransforms.Keys );
				_rotActiveAxis = GizmoAxis.None;
				_rotDragStartTransforms.Clear();
				_rotDragStartTransformsCloud.Clear();
				_rotDragBasisOverride = null;
				_undo.EndEdit();
				return false;
			}

			if ( IntersectRayRingPlane( cameraRay, _rotActiveAxis, out var hitPoint ) )
			{
				var dir = GizmoAxisDir( _rotActiveAxis );
				var radial = hitPoint - _rotDragPivot;
				radial -= dir * Vector3.Dot( radial, dir );
				if ( radial.LengthSquared > _radialEpsilon )
				{
					var current = radial.Normal;
					var (u, v) = GizmoRingBasis( _rotActiveAxis );
					var startAngle = MathF.Atan2( Vector3.Dot( _rotDragStartDir, v ), Vector3.Dot( _rotDragStartDir, u ) );
					var currentAngle = MathF.Atan2( Vector3.Dot( current, v ), Vector3.Dot( current, u ) );
					var deltaDeg = (currentAngle - startAngle) * 180f / MathF.PI;
					deltaDeg = SnapAngle( deltaDeg );
					var rotation = Rotation.FromAxis( dir, deltaDeg );

					foreach ( var (brush, start) in _rotDragStartTransforms )
					{
						if ( !brush.IsValid() ) continue;
						brush.WorldRotation = rotation * start.Rotation;
						var offset = start.Position - _rotDragPivot;
						brush.WorldPosition = _rotDragPivot + offset * rotation;
					}
					foreach ( var (ci, start) in _rotDragStartTransformsCloud )
					{
						if ( !ci.IsValid() ) continue;
						ci.WorldRotation = rotation * start.Rotation;
						var offset = start.Position - _rotDragPivot;
						ci.WorldPosition = _rotDragPivot + offset * rotation;
					}
				}
			}
			return true;
		}

		if ( !TraceRotateGizmo( cameraRay, out var hovered, out var hoverPoint ) )
		{
			_rotHoveredAxis = GizmoAxis.None;
			return false;
		}
		_rotHoveredAxis = hovered;

		if ( Input.Pressed( "attack1" ) )
		{
			_rotActiveAxis = hovered;
			_rotDragPivot = _rotGizmoCenter;
			// Snapshot the local-axis basis (if any) BEFORE we read axis
			// directions so every drag-frame stays aligned to the brush's
			// rotation at click-time.
			_rotDragBasisOverride = LocalGizmo ? GizmoBasis() : (Rotation?)null;
			var dir = GizmoAxisDir( hovered );
			var radial = hoverPoint - _rotDragPivot;
			radial -= dir * Vector3.Dot( radial, dir );
			_rotDragStartDir = radial.LengthSquared > _radialEpsilon ? radial.Normal : GizmoRingBasis( hovered ).U;

			_rotDragStartTransforms.Clear();
			var dragBrushes = CollectSelectedBrushes().ToList();
			foreach ( var brush in dragBrushes )
			{
				brush.TakeOwnershipForEdit();
				_rotDragStartTransforms[brush] = (brush.WorldPosition, brush.WorldRotation);
			}
			_rotDragStartTransformsCloud.Clear();
			var dragCloud = _selectedCloud.Where( c => c.IsValid() ).ToList();
			foreach ( var ci in dragCloud )
			{
				_rotDragStartTransformsCloud[ci] = (ci.WorldPosition, ci.WorldRotation);
			}
			_undo.BeginEdit( dragBrushes );
			foreach ( var ci in dragCloud )
			{
				_undo.TrackExistingCloud( ci );
			}
		}
		return true;
	}

	private bool TraceRotateGizmo( Ray cameraRay, out GizmoAxis axis, out Vector3 hitPoint )
	{
		axis = GizmoAxis.None;
		hitPoint = _rotGizmoCenter;
		var bestDist = float.PositiveInfinity;
		var radius = ScaledRingRadius;
		var hitRadius = ScaledRingHitRadius;

		for ( var a = GizmoAxis.X; a <= GizmoAxis.Z; a++ )
		{
			if ( !IntersectRayRingPlane( cameraRay, a, out var p ) ) continue;
			var radial = (p - _rotGizmoCenter).Length;
			if ( MathF.Abs( radial - radius ) > hitRadius ) continue;
			var rayDist = Vector3.Dot( p - cameraRay.Position, cameraRay.Forward );
			if ( rayDist < 0f || rayDist >= bestDist ) continue;
			bestDist = rayDist;
			axis = a;
			hitPoint = p;
		}
		return axis != GizmoAxis.None;
	}

	private bool IntersectRayRingPlane( Ray ray, GizmoAxis axis, out Vector3 point )
	{
		var dir = GizmoAxisDir( axis );
		var denom = Vector3.Dot( ray.Forward, dir );
		if ( MathF.Abs( denom ) < _axisEpsilon )
		{
			point = _rotGizmoCenter;
			return false;
		}
		var t = Vector3.Dot( _rotGizmoCenter - ray.Position, dir ) / denom;
		if ( t < 0f || t > MaxRayDistance )
		{
			point = _rotGizmoCenter;
			return false;
		}
		point = ray.Position + ray.Forward * t;
		return true;
	}

	#endregion

	#region ScaleGizmo

	private static readonly GizmoAxisSide[] _allScaleSides =
	{
		GizmoAxisSide.PosX, GizmoAxisSide.NegX,
		GizmoAxisSide.PosY, GizmoAxisSide.NegY,
		GizmoAxisSide.PosZ, GizmoAxisSide.NegZ,
	};

	private bool TryGetSelectionWorldBounds( out BBox bounds )
	{
		var min = new Vector3( float.PositiveInfinity, float.PositiveInfinity, float.PositiveInfinity );
		var max = new Vector3( float.NegativeInfinity, float.NegativeInfinity, float.NegativeInfinity );
		var any = false;
		foreach ( var brush in CollectSelectedBrushes() )
		{
			if ( !brush.IsValid() ) continue;
			ExpandWorldBounds( brush.Bounds, brush.WorldTransform, ref min, ref max );
			any = true;
		}
		foreach ( var ci in _selectedCloud )
		{
			if ( !ci.IsValid() ) continue;
			if ( ci.IsEntityPlaceholder ) continue;
			if ( !TryGetCloudLocalBounds( ci, out var local ) ) continue;
			ExpandWorldBounds( local, ci.WorldTransform, ref min, ref max );
			any = true;
		}
		bounds = any ? new BBox( min, max ) : default;
		return any;
	}

	private static void ExpandWorldBounds( BBox local, Transform transform, ref Vector3 min, ref Vector3 max )
	{
		for ( var i = 0; i < 8; i++ )
		{
			var corner = new Vector3(
				(i & 1) != 0 ? local.Maxs.x : local.Mins.x,
				(i & 2) != 0 ? local.Maxs.y : local.Mins.y,
				(i & 4) != 0 ? local.Maxs.z : local.Mins.z );
			var world = transform.PointToWorld( corner );
			min = Vector3.Min( min, world );
			max = Vector3.Max( max, world );
		}
	}

	private static bool TryGetCloudLocalBounds( CloudInstance ci, out BBox bounds )
	{
		bounds = default;
		var renderer = ci.Renderer;
		if ( renderer is null || !renderer.IsValid() )
		{
			renderer = ci.GameObject?.GetComponent<ModelRenderer>();
			if ( renderer is null ) return false;
		}
		var model = renderer.Model;
		if ( model is null ) return false;
		bounds = model.Bounds;
		return true;
	}

	private static GizmoAxis SideAxis( GizmoAxisSide side ) => side switch
	{
		GizmoAxisSide.PosX or GizmoAxisSide.NegX => GizmoAxis.X,
		GizmoAxisSide.PosY or GizmoAxisSide.NegY => GizmoAxis.Y,
		GizmoAxisSide.PosZ or GizmoAxisSide.NegZ => GizmoAxis.Z,
		_ => GizmoAxis.None,
	};

	private static int SideAxisIndex( GizmoAxisSide side ) => SideAxis( side ) switch
	{
		GizmoAxis.X => 0,
		GizmoAxis.Y => 1,
		GizmoAxis.Z => 2,
		_ => -1,
	};

	private static float SideSign( GizmoAxisSide side ) => side switch
	{
		GizmoAxisSide.PosX or GizmoAxisSide.PosY or GizmoAxisSide.PosZ => 1f,
		GizmoAxisSide.NegX or GizmoAxisSide.NegY or GizmoAxisSide.NegZ => -1f,
		_ => 0f,
	};

	private static GizmoAxisSide OppositeSide( GizmoAxisSide side ) => side switch
	{
		GizmoAxisSide.PosX => GizmoAxisSide.NegX,
		GizmoAxisSide.NegX => GizmoAxisSide.PosX,
		GizmoAxisSide.PosY => GizmoAxisSide.NegY,
		GizmoAxisSide.NegY => GizmoAxisSide.PosY,
		GizmoAxisSide.PosZ => GizmoAxisSide.NegZ,
		GizmoAxisSide.NegZ => GizmoAxisSide.PosZ,
		_ => GizmoAxisSide.None,
	};

	private static Color SideColor( GizmoAxisSide side ) => SideAxis( side ) switch
	{
		GizmoAxis.X => _gizmoXColor,
		GizmoAxis.Y => _gizmoYColor,
		GizmoAxis.Z => _gizmoZColor,
		_ => Color.White,
	};

	private Vector3 FrameHandlePos( GizmoAxisSide side )
	{
		var i = SideAxisIndex( side );
		if ( i < 0 ) return _scaleCenter;
		return _scaleCenter + _scaleAxes[i] * (_scaleExtents[i] * SideSign( side ));
	}

	private Vector3 FrameSideDir( GizmoAxisSide side )
	{
		var i = SideAxisIndex( side );
		if ( i < 0 ) return Vector3.Zero;
		return _scaleAxes[i] * SideSign( side );
	}

	private bool TryBuildScaleFrame()
	{
		// Vertex mode builds a world-aligned AABB around the picked vertices
		// and scales each one relative to the opposite handle (always global —
		// the LOCAL toggle stays grey'd out because LocalGizmoApplies returns
		// false outside single-brush / single-entity selections).
		if ( Selection == SelectionMode.Vertex )
		{
			return BuildVerticesAABBFrame();
		}

		var brushes = CollectSelectedBrushes().Where( b => b.IsValid() ).ToList();
		var cloudModels = _selectedCloud
			.Where( c => c.IsValid() && !c.IsEntityPlaceholder )
			.ToList();
		var total = brushes.Count + cloudModels.Count;
		if ( total == 0 ) return false;

		if ( total == 1 )
		{
			// Single shape: always align the frame with its own OBB so the
			// gizmo follows the shape's local axes. No LOCAL toggle for
			// Scale — the toolbar grey's it out for the entire Scale tool
			// (see ScaleGateClass / LocalGizmoGateClass).
			if ( brushes.Count == 1 )
			{
				var brush = brushes[0];
				BuildSingleOBBFrame( brush.Bounds, brush.WorldTransform, brush.WorldScale );
			}
			else
			{
				var ci = cloudModels[0];
				if ( !TryGetCloudLocalBounds( ci, out var local ) ) return false;
				BuildSingleOBBFrame( local, ci.WorldTransform, ci.WorldScale );
			}
			return true;
		}

		// Multi-selection is locked to a world-aligned frame — there's no
		// single shared local basis to fall back to, and the LOCAL toggle
		// is grey'd out in the toolbar in this case (LocalGizmoApplies is
		// false). ApplyAxisScale still stretches each shape along its OWN
		// local axes, so rotated brushes will grow in their own local
		// directions even though the gizmo handles point along world axes.
		return BuildWorldAABBFrame();
	}

	private bool BuildWorldAABBFrame()
	{
		if ( !TryGetSelectionWorldBounds( out var bounds ) ) return false;
		_scaleCenter = bounds.Center;
		_scaleAxes[0] = Vector3.Forward;
		_scaleAxes[1] = Vector3.Left;
		_scaleAxes[2] = Vector3.Up;
		var halfSize = bounds.Size * 0.5f;
		_scaleExtents[0] = MathF.Abs( halfSize.x );
		_scaleExtents[1] = MathF.Abs( halfSize.y );
		_scaleExtents[2] = MathF.Abs( halfSize.z );
		return true;
	}

	// World-AABB around every selected vertex's current world position.
	// Used by the Scale gizmo when Vertex mode is the active selection.
	private bool BuildVerticesAABBFrame()
	{
		if ( _selectedVertices.Count < 2 ) return false;
		var min = new Vector3( float.PositiveInfinity, float.PositiveInfinity, float.PositiveInfinity );
		var max = new Vector3( float.NegativeInfinity, float.NegativeInfinity, float.NegativeInfinity );
		var any = false;
		foreach ( var (brush, vIdx) in _selectedVertices )
		{
			if ( brush is null || !brush.IsValid() ) continue;
			if ( !brush.TryGetVertexWorldPosition( vIdx, out var world ) ) continue;
			min = Vector3.Min( min, world );
			max = Vector3.Max( max, world );
			any = true;
		}
		if ( !any ) return false;
		_scaleCenter = (min + max) * 0.5f;
		_scaleAxes[0] = Vector3.Forward;
		_scaleAxes[1] = Vector3.Left;
		_scaleAxes[2] = Vector3.Up;
		var halfSize = (max - min) * 0.5f;
		_scaleExtents[0] = MathF.Abs( halfSize.x );
		_scaleExtents[1] = MathF.Abs( halfSize.y );
		_scaleExtents[2] = MathF.Abs( halfSize.z );
		return true;
	}

	private void BuildSingleOBBFrame( BBox localBounds, Transform worldTransform, Vector3 worldScale )
	{
		_scaleCenter = worldTransform.PointToWorld( localBounds.Center );
		_scaleAxes[0] = (Vector3.Forward * worldTransform.Rotation).Normal;
		_scaleAxes[1] = (Vector3.Left * worldTransform.Rotation).Normal;
		_scaleAxes[2] = (Vector3.Up * worldTransform.Rotation).Normal;
		_scaleExtents[0] = MathF.Abs( localBounds.Size.x * 0.5f * worldScale.x );
		_scaleExtents[1] = MathF.Abs( localBounds.Size.y * 0.5f * worldScale.y );
		_scaleExtents[2] = MathF.Abs( localBounds.Size.z * 0.5f * worldScale.z );
	}

	private void UpdateScaleGizmo()
	{
		if ( !IsScaleTransformable() || CurrentTool != Tool.Scale )
		{
			_scaleGizmoVisible = false;
			_scaleHoveredSide = GizmoAxisSide.None;
			return;
		}

		if ( _scaleActiveSide == GizmoAxisSide.None )
		{
			if ( !TryBuildScaleFrame() )
			{
				_scaleGizmoVisible = false;
				_scaleHoveredSide = GizmoAxisSide.None;
				return;
			}
			_scaleCurrentFactor = 1f;
		}

		_scaleGizmoVisible = true;

		var distance = Vector3.DistanceBetween( Scene.Camera.WorldPosition, _scaleCenter );
		_scaleGizmoScale = MathF.Max( _gizmoMinScale, distance / _gizmoReferenceDistance );

		foreach ( var side in _allScaleSides )
		{
			DrawGizmoScaleHandle( side );
		}
	}

	private Vector3 ScaleHandleVisualPos( GizmoAxisSide side )
	{
		if ( _scaleActiveSide == side )
		{
			return _scaleDragPivot + _scaleDragAxisDir * (_scaleStartSize * _scaleCurrentFactor);
		}
		return FrameHandlePos( side );
	}

	private void DrawGizmoScaleHandle( GizmoAxisSide side )
	{
		var color = (side == _scaleHoveredSide || side == _scaleActiveSide) ? _gizmoHighlightColor : SideColor( side );
		var origin = _scaleCenter;
		var handlePos = ScaleHandleVisualPos( side );
		CustomOverlay.Line( origin, handlePos, color, 0f, default, true, GizmoLineThickness );

		var handleHalf = ScaledScaleHandleSize;
		var handleExtents = new Vector3( handleHalf, handleHalf, handleHalf );
		var bbox = new BBox( handlePos - handleExtents, handlePos + handleExtents );
		CustomOverlay.Box( bbox, color, 0f, default, true, GizmoLineThickness );
	}

	private bool UpdateScaleGizmoDrag( Ray cameraRay )
	{
		if ( !_scaleGizmoVisible ) return false;

		var isVertexScale = Selection == SelectionMode.Vertex;

		if ( _scaleActiveSide != GizmoAxisSide.None )
		{
			if ( !Input.Down( "attack1" ) )
			{
				if ( isVertexScale )
				{
					// MoveVerticesLocal already mutated the meshes locally each
					// tick; broadcast the final shape so peers catch up.
					var brushes = _vertexDragStartLocal.Keys
						.Select( k => k.brush )
						.Where( b => b is not null && b.IsValid() )
						.Distinct()
						.ToList();
					PublishUVSnapshotToPeers( brushes );
					_vertexDragStartLocal.Clear();
				}
				else
				{
					PublishUVSnapshotToPeers( _scaleDragStartTransforms.Keys );
				}
				_scaleActiveSide = GizmoAxisSide.None;
				_scaleDragStartTransforms.Clear();
				_scaleDragStartTransformsCloud.Clear();
				_scaleCurrentFactor = 1f;
				_undo.EndEdit();
				return false;
			}

			if ( ClosestPointOnAxis( cameraRay, _scaleDragPivot, _scaleDragAxisDir, out var current ) )
			{
				var pivotAlong = Vector3.Dot( _scaleDragPivot, _scaleDragAxisDir );
				var rawAlong = Vector3.Dot( current, _scaleDragAxisDir );
				if ( _gridEnabled && _gridSize > 0f )
				{
					rawAlong = MathF.Round( rawAlong / _gridSize ) * _gridSize;
				}
				var newSize = rawAlong - pivotAlong;
				var factor = _scaleStartSize > _radialEpsilon ? newSize / _scaleStartSize : 1f;
				factor = MathF.Max( _minScaleFactor, factor );
				_scaleCurrentFactor = factor;

				if ( isVertexScale )
				{
					ApplyVertexScale( factor );
				}
				else
				{
					var axis = SideAxis( _scaleActiveSide );
					foreach ( var (brush, start) in _scaleDragStartTransforms )
					{
						if ( !brush.IsValid() ) continue;
						ApplyAxisScale( axis, factor, start.Scale, start.Position, out var newScale, out var newPos );
						brush.WorldScale = newScale;
						brush.WorldPosition = newPos;
					}
					foreach ( var (ci, start) in _scaleDragStartTransformsCloud )
					{
						if ( !ci.IsValid() ) continue;
						ApplyAxisScale( axis, factor, start.Scale, start.Position, out var newScale, out var newPos );
						ci.WorldScale = newScale;
						ci.WorldPosition = newPos;
					}
				}
			}
			return true;
		}

		if ( !TraceScaleGizmo( cameraRay, out var hovered ) )
		{
			_scaleHoveredSide = GizmoAxisSide.None;
			return false;
		}
		_scaleHoveredSide = hovered;

		if ( Input.Pressed( "attack1" ) )
		{
			_scaleActiveSide = hovered;
			_scaleDragAxisDir = FrameSideDir( hovered );
			_scaleDragPivot = FrameHandlePos( OppositeSide( hovered ) );
			var handle = FrameHandlePos( hovered );
			_scaleStartSize = Vector3.Dot( handle - _scaleDragPivot, _scaleDragAxisDir );
			_scaleCurrentFactor = 1f;

			_scaleDragStartTransforms.Clear();
			_scaleDragStartTransformsCloud.Clear();

			if ( isVertexScale )
			{
				// Vertex scale keeps brushes' transforms put — only the per-
				// vertex local positions change — so we don't populate the
				// transform dictionaries. Mirror the move-tool's vertex drag:
				// snapshot each vertex's local position so subsequent ticks
				// rescale against a stable anchor (MoveVerticesLocal rebuilds
				// the mesh and reassigns vertex indices each call; RemapVertex
				// Keys keeps the keys aligned).
				_vertexDragStartLocal.Clear();
				var dragBrushes = new HashSet<ModelBrush>();
				foreach ( var (brush, vIdx) in _selectedVertices )
				{
					if ( brush is null || !brush.IsValid() ) continue;
					if ( !brush.CanLocalEdit() ) continue;
					if ( !brush.TryGetVertexLocalPosition( vIdx, out var local ) ) continue;
					_vertexDragStartLocal[(brush, vIdx)] = local;
					dragBrushes.Add( brush );
				}
				foreach ( var brush in dragBrushes ) brush.TakeOwnershipForEdit();
				_undo.BeginEdit( dragBrushes );
			}
			else
			{
				var dragBrushes = CollectSelectedBrushes().ToList();
				foreach ( var brush in dragBrushes )
				{
					brush.TakeOwnershipForEdit();
					_scaleDragStartTransforms[brush] = (brush.WorldPosition, brush.WorldScale);
				}
				var dragCloud = _selectedCloud.Where( c => c.IsValid() && !c.IsEntityPlaceholder ).ToList();
				foreach ( var ci in dragCloud )
				{
					_scaleDragStartTransformsCloud[ci] = (ci.WorldPosition, ci.WorldScale);
				}
				_undo.BeginEdit( dragBrushes );
				foreach ( var ci in dragCloud )
				{
					_undo.TrackExistingCloud( ci );
				}
			}
		}
		return true;
	}

	// Per-vertex world-space scale around _scaleDragPivot (the opposite
	// handle), restricted to the active drag axis _scaleDragAxisDir. Same
	// formula ApplyAxisScale uses on a brush's WorldPosition: keep the
	// vertex's perpendicular offset, scale only the component along the
	// drag axis. Always operates in global space — there is no shared
	// local frame for a multi-brush vertex selection.
	private void ApplyVertexScale( float factor )
	{
		if ( _vertexDragStartLocal.Count == 0 ) return;

		var byBrush = new Dictionary<ModelBrush, Dictionary<int, Vector3>>();
		foreach ( var ((brush, vIdx), startLocal) in _vertexDragStartLocal )
		{
			if ( brush is null || !brush.IsValid() ) continue;
			if ( !brush.CanLocalEdit() ) continue;

			var startWorld = brush.WorldTransform.PointToWorld( startLocal );
			var offset = startWorld - _scaleDragPivot;
			var alongComp = Vector3.Dot( offset, _scaleDragAxisDir );
			var newAlong = alongComp * factor;
			var newWorld = _scaleDragPivot + offset + _scaleDragAxisDir * (newAlong - alongComp);
			var newLocal = brush.WorldTransform.PointToLocal( newWorld );

			if ( !byBrush.TryGetValue( brush, out var moves ) )
			{
				moves = new Dictionary<int, Vector3>();
				byBrush[brush] = moves;
			}
			moves[vIdx] = newLocal;
		}

		foreach ( var (brush, moves) in byBrush )
		{
			var remap = brush.MoveVerticesLocal( moves );
			if ( remap is null || remap.Count == 0 ) continue;
			RemapVertexKeys( brush, remap );
		}
	}

	private void ApplyAxisScale( GizmoAxis axis, float factor, Vector3 startScale, Vector3 startPos,
		out Vector3 newScale, out Vector3 newPos )
	{
		newScale = startScale;
		switch ( axis )
		{
			case GizmoAxis.X: newScale.x = startScale.x * factor; break;
			case GizmoAxis.Y: newScale.y = startScale.y * factor; break;
			case GizmoAxis.Z: newScale.z = startScale.z * factor; break;
		}

		var offset = startPos - _scaleDragPivot;
		var alongComp = Vector3.Dot( offset, _scaleDragAxisDir );
		var newAlong = alongComp * factor;
		newPos = _scaleDragPivot + offset + _scaleDragAxisDir * (newAlong - alongComp);
	}

	private bool TraceScaleGizmo( Ray cameraRay, out GizmoAxisSide side )
	{
		side = GizmoAxisSide.None;
		var rayDir = cameraRay.Forward;
		var bestDist = float.PositiveInfinity;
		var handleHalf = ScaledScaleHandleSize;
		var hitRadius = MathF.Max( ScaledScaleHitRadius, handleHalf );
		var origin = _scaleCenter;

		foreach ( var s in _allScaleSides )
		{
			var dir = FrameSideDir( s );
			var handle = FrameHandlePos( s );
			var endPoint = handle + dir * handleHalf;
			if ( !ClosestRayToSegment( cameraRay.Position, rayDir, MaxRayDistance, origin, endPoint,
				out var rayDistance, out var separation ) )
				continue;
			if ( separation > hitRadius ) continue;
			if ( rayDistance >= bestDist ) continue;
			bestDist = rayDistance;
			side = s;
		}
		return side != GizmoAxisSide.None;
	}

	#endregion

	#region Slice Tool

	private void UpdateSliceTool( Ray ray )
	{
		if ( !CanSlice )
		{
			_sliceDragging = false;
			_sliceHasPick = false;
			CurrentTool = Tool.Move;
			return;
		}

		// Slice operates on every currently-selected brush. Outline AND fill
		// each one so the user can see which targets are about to be cut at
		// a glance, including faces hidden behind other geometry.
		var sliceTargets = _selectedBrushes.Where( b => b.IsValid() ).ToList();
		foreach ( var b in sliceTargets )
		{
			b.Hover( true );
			DrawSelectedBrushFill( b );
		}
		DrawHoveredBrushOutline( ray );

		if ( !_sliceDragging )
		{
			if ( TryGetSlicePickPoint( ray, sliceTargets, out var pick, out var pickNormal ) )
			{
				AlignToGrid( ref pick );
				_slicePickPoint = pick;
				_slicePickNormal = pickNormal;
				_sliceHasPick = true;
				DrawSliceGizmoBox( pick );
			}
			else
			{
				_sliceHasPick = false;
			}

			if ( _sliceHasPick && Input.Pressed( "attack1" ) )
			{
				_sliceStartPoint = _slicePickPoint;
				_sliceCurrentPoint = _slicePickPoint;
				_sliceSurfaceNormal = _slicePickNormal;
				// Seed the projection plane with the initial pick's face
				// so frames before any other face is hovered still project
				// onto a sensible surface.
				_sliceDragSurfacePlane = new Plane( _slicePickPoint, _slicePickNormal );
				_sliceDragging = true;
			}
			return;
		}

		// During the drag the second point lives on the plane of the last
		// brush face the cursor passed over. As the user sweeps across
		// different faces the projection plane updates; when the cursor is
		// in open space we keep using the most recent one (or the initial
		// pick's plane, if nothing else has been hovered yet).
		var dragTrace = Scene.SceneWorld.Trace.Ray( ray, MaxRayDistance ).WithoutTags( "player" ).Run();
		if ( dragTrace.Hit
			&& dragTrace.SceneObject?.GetGameObject()?.GetComponent<ModelBrush>() is not null
			&& dragTrace.Normal.LengthSquared > _axisEpsilon )
		{
			_sliceDragSurfacePlane = new Plane( dragTrace.HitPosition, dragTrace.Normal.Normal );
		}
		var hit = _sliceDragSurfacePlane.IntersectLine( ray.Position, ray.Position + ray.Forward * MaxRayDistance );
		if ( hit.HasValue )
		{
			var p = hit.Value;
			AlignToGrid( ref p );
			// Grid alignment can knock the point off the plane; project it
			// back so the second point always lies on the projection plane.
			p = ProjectOntoPlane( p, _sliceDragSurfacePlane );
			_sliceCurrentPoint = p;
		}

		// Shift snaps the slice direction to the closest world cardinal axis
		// (±X/±Y/±Z): project the start→current vector onto the dominant axis
		// so the second point slides along that axis instead of the hovered
		// surface. Mirrors the plane-normal snap in TryBuildSlicePlane so the
		// preview line matches the axis-aligned plane that gets committed.
		if ( Input.Down( "Run" ) )
		{
			var dir = _sliceCurrentPoint - _sliceStartPoint;
			var axis = SnapToCardinalAxis( dir );
			var d = Vector3.Dot( dir, axis );
			if ( _gridEnabled && _gridSize > 0f )
			{
				d = MathF.Round( d / _gridSize ) * _gridSize;
			}
			_sliceCurrentPoint = _sliceStartPoint + axis * d;
		}

		DrawSliceGizmoBox( _sliceStartPoint );
		DrawSliceGizmoBox( _sliceCurrentPoint );
		CustomOverlay.Line( _sliceStartPoint, _sliceCurrentPoint, _sliceGizmoColor, 0f, default, true, OutlineLineThickness );

		if ( Input.Released( "attack1" ) )
		{
			_sliceDragging = false;
			if ( TryBuildSlicePlane( out var commitPlane ) )
			{
				ApplySlice( sliceTargets, commitPlane );
			}
		}
	}

	private bool TryGetSlicePickPoint( Ray ray, IReadOnlyCollection<ModelBrush> brushes, out Vector3 point, out Vector3 normal )
	{
		var brushTrace = Scene.SceneWorld.Trace.Ray( ray, MaxRayDistance ).WithoutTags( "player" ).Run();
		if ( brushTrace.Hit )
		{
			var hitBrush = brushTrace.SceneObject?.GetGameObject()?.GetComponent<ModelBrush>();
			if ( hitBrush is not null && brushes.Contains( hitBrush ) )
			{
				point = brushTrace.HitPosition;
				normal = brushTrace.Normal.LengthSquared > _axisEpsilon ? brushTrace.Normal.Normal : Vector3.Up;
				return true;
			}
		}

		var fallback = new Plane( PlaneNormal * PlaneDistance, PlaneNormal );
		var fb = fallback.IntersectLine( ray.Position, ray.Position + ray.Forward * MaxRayDistance );
		if ( fb.HasValue )
		{
			point = fb.Value;
			normal = PlaneNormal.LengthSquared > _axisEpsilon ? PlaneNormal.Normal : Vector3.Up;
			return true;
		}
		point = default;
		normal = Vector3.Up;
		return false;
	}

	private static Vector3 ProjectOntoPlane( Vector3 point, Plane plane )
	{
		var d = plane.GetDistance( point );
		return point - plane.Normal * d;
	}

	private void DrawSliceGizmoBox( Vector3 worldPos )
	{
		CustomOverlay.Box( worldPos, Vector3.One * _sliceGizmoSize, _sliceGizmoColor, 0f, default, true, OutlineLineThickness );
	}

	private bool TryBuildSlicePlane( out Plane plane )
	{
		plane = default;
		var right = _sliceCurrentPoint - _sliceStartPoint;
		if ( right.Length < _sliceMinDragDistance ) return false;
		var up = _sliceSurfaceNormal.LengthSquared > _axisEpsilon ? _sliceSurfaceNormal.Normal : Vector3.Up;
		var normal = Vector3.Cross( up, right ).Normal;
		if ( normal.LengthSquared < _axisEpsilon ) return false;
		// Shift forces the plane normal onto the closest world cardinal axis
		// (±X / ±Y / ±Z), so the resulting slice plane is always axis-aligned
		// even when the picked surface or drag direction is rotated.
		if ( Input.Down( "Run" ) )
		{
			normal = SnapToCardinalAxis( normal );
		}
		plane = new Plane( _sliceStartPoint, normal );
		return true;
	}

	private static Vector3 SnapToCardinalAxis( Vector3 v )
	{
		var ax = MathF.Abs( v.x );
		var ay = MathF.Abs( v.y );
		var az = MathF.Abs( v.z );
		if ( ax >= ay && ax >= az ) return new Vector3( MathF.Sign( v.x ), 0f, 0f );
		if ( ay >= az ) return new Vector3( 0f, MathF.Sign( v.y ), 0f );
		return new Vector3( 0f, 0f, MathF.Sign( v.z ) );
	}

	private void BroadcastMeshSnapshot( ModelBrush brush, string errorContext )
	{
		byte[] snapshot = null;
		try
		{
			snapshot = brush.SerializeMeshState();
		}
		catch ( Exception e )
		{
			Log.Warning( $"togethercsg: {errorContext} serialize failed: {e.Message}" );
		}
		if ( Networking.IsActive && snapshot is not null )
		{
			using ( Rpc.FilterExclude( Connection.Local ) )
			{
				brush.RpcApplyMeshSnapshot( snapshot );
			}
		}
	}

	private void ApplySlice( IReadOnlyList<ModelBrush> brushes, Plane worldPlane )
	{
		var targets = brushes.Where( b => b.IsValid() && b.CanLocalEdit() ).ToList();
		if ( targets.Count == 0 ) return;

		_undo.BeginEdit( targets );

		var anySliced = false;
		foreach ( var brush in targets )
		{
			if ( TrySliceBrush( brush, worldPlane ) ) anySliced = true;
		}

		if ( !anySliced )
		{
			_undo.CancelEdit();
			return;
		}

		_undo.EndEdit();

		ClearAllSelections();
		CurrentTool = Tool.Move;
	}

	// Apply the world-space slice plane to a single brush: clip the front
	// half in-place, spawn a new brush for the back half. Returns false
	// when the plane misses (or merely grazes) this brush so the caller
	// knows nothing happened here — the brush remains untouched in that
	// case.
	private bool TrySliceBrush( ModelBrush brush, Plane worldPlane )
	{
		var transform = brush.WorldTransform;
		var n = worldPlane.Normal;

		var tangent = MathF.Abs( n.z ) < _planeCrossThreshold
			? Vector3.Cross( n, Vector3.Up )
			: Vector3.Cross( n, Vector3.Forward );

		var bitangent = Vector3.Cross( n, tangent );

		var p0 = worldPlane.Position;
		var p1 = p0 + tangent * _sliceTangentSpan;
		var p2 = p0 + bitangent * _sliceTangentSpan;

		var lp0 = transform.PointToLocal( p0 );
		var lp1 = transform.PointToLocal( p1 );
		var lp2 = transform.PointToLocal( p2 );

		var planeFront = new Plane( lp0, lp1, lp2 );
		var planeBack = new Plane( -planeFront.Normal, -planeFront.Distance );

		// Pre-clip validation: the slice plane must actually cross the
		// brush's local-space bounding box, with both sides containing
		// vertices well clear of the plane. Plane-grazes-the-brush cases
		// (one half empty or a sliver-thin shard) used to make
		// ClipFacesByPlaneAndCap produce degenerate output that crashed
		// the engine on render. Rejecting upfront is safe because the
		// live mesh hasn't been touched yet — no rollback needed and no
		// risk of disturbing the engine state cap-face synthesis relies
		// on.
		if ( !SlicePlaneCrossesBrush( brush.PolygonMesh, planeFront ) ) return false;

		PolygonMesh backMesh;
		try
		{
			backMesh = ClonePolygonMesh( brush.PolygonMesh );
		}
		catch ( Exception e )
		{
			Log.Warning( $"togethercsg: slice clone failed: {e.Message}" );
			return false;
		}
		backMesh.SetTransform( transform );

		var frontMesh = brush.PolygonMesh;

		WeldCoincidentVertices( frontMesh, _sliceWeldEpsilon );
		WeldCoincidentVertices( backMesh, _sliceWeldEpsilon );

		// Clip without engine-side cap synthesis. The engine's FaceCutter is per-
		// face and emits a fresh vertex on each cut edge per face, so on the side-
		// wall edge shared between two adjacent faces it produces two coincident-
		// but-disconnected boundary verts. CreateFaceInEdgeLoop then can't close
		// the loop and silently emits no cap (user reports the two halves with no
		// cap, especially when the cut plane runs along an existing ring edge —
		// e.g. slicing a primitive right at a side-wall bottom ring). We do the
		// cut here, post-weld to merge those boundary duplicates into proper
		// shared verts, then trace and synthesise the cap loop ourselves below.
		var frontFaces = frontMesh.FaceHandles.ToList();
		frontMesh.ClipFacesByPlaneAndCap( frontFaces, planeFront, true, false, null, null );

		var backFaces = backMesh.FaceHandles.ToList();
		backMesh.ClipFacesByPlaneAndCap( backFaces, planeBack, true, false, null, null );

		WeldCoincidentVertices( frontMesh, _sliceWeldEpsilon );
		WeldCoincidentVertices( backMesh, _sliceWeldEpsilon );

		var frontCaps = SynthesizeCapFaces( frontMesh, planeFront, _sliceCapPlaneEpsilon );
		var backCaps = SynthesizeCapFaces( backMesh, planeBack, _sliceCapPlaneEpsilon );

		if ( !frontMesh.FaceHandles.Any() || !backMesh.FaceHandles.Any() )
		{
			// SlicePlaneCrossesBrush should have caught this; if it
			// didn't, frontMesh is now partially clipped and we can't
			// cleanly back out. Stop touching the brush, accept the
			// in-place state, and let the caller's undo entry roll
			// things back if needed.
			return false;
		}

		AlignCapFaces( frontMesh, frontCaps, brush.WorldRotation );
		AlignCapFaces( backMesh, backCaps, brush.WorldRotation );

		frontMesh.ComputeFaceTextureCoordinatesFromParameters();
		backMesh.ComputeFaceTextureCoordinatesFromParameters();

		brush.ReplacePolygonMesh( frontMesh );
		BroadcastMeshSnapshot( brush, "slice front" );

		var prefab = _cubePrefab;
		if ( prefab is null ) return true;

		var backObject = prefab.Clone();
		backObject.WorldTransform = transform;
		if ( Networking.IsActive )
		{
			backObject.NetworkSpawn();
			// NetworkSpawn() makes the slicer the owner. When the host slices a
			// peer-owned brush the front piece stays peer-owned, so the back
			// piece needs to follow suit — otherwise the peer can't delete it.
			try
			{
				var frontNet = brush.GameObject?.Network;
				if ( frontNet is { Active: true } && frontNet.Owner is not null )
				{
					backObject.Network.AssignOwnership( frontNet.Owner );
				}
			}
			catch ( Exception e )
			{
				Log.Warning( $"togethercsg: slice ownership transfer failed: {e.Message}" );
			}
		}
		var backBrush = backObject.GetComponent<ModelBrush>();
		if ( backBrush is null )
		{
			backObject.Destroy();
			return true;
		}

		backBrush.ReplacePolygonMesh( backMesh );
		BroadcastMeshSnapshot( backBrush, "slice back" );
		_undo.TrackNewBrush( backBrush );
		return true;
	}

	// Returns true when both sides of `localPlane` contain at least one
	// brush vertex sitting clear of the plane (by more than _sliceMinCrossDist).
	// False when the plane misses the brush or just barely grazes a corner —
	// those are the cases that previously fed ClipFacesByPlaneAndCap a
	// degenerate cut and produced invalid engine output. Threshold is in
	// the brush's local space (where stock primitives have half-extents of
	// 0.5), so 0.01 is roughly a 2% relative margin.
	private const float _sliceMinCrossDist = 0.01f;
	private static bool SlicePlaneCrossesBrush( PolygonMesh mesh, Plane localPlane )
	{
		if ( mesh is null ) return false;
		var hasFront = false;
		var hasBack = false;
		foreach ( var v in mesh.VertexHandles )
		{
			var d = localPlane.GetDistance( mesh.GetVertexPosition( v ) );
			if ( d > _sliceMinCrossDist ) hasFront = true;
			else if ( d < -_sliceMinCrossDist ) hasBack = true;
			if ( hasFront && hasBack ) return true;
		}
		return false;
	}

	private static PolygonMesh ClonePolygonMesh( PolygonMesh source )
	{
		var dst = new PolygonMesh();
		dst.MergeMesh( source, global::Transform.Zero, out _, out _, out _ );
		return dst;
	}

	private static void WeldCoincidentVertices( PolygonMesh mesh, float epsilon )
	{
		if ( mesh is null ) return;
		var verts = mesh.VertexHandles.ToList();
		if ( verts.Count < 2 ) return;
		mesh.MergeVerticesWithinDistance( verts, epsilon, bPreConnect: false, bAveragePositions: true, out _ );
	}

	// Replacement for the engine's per-face cap loop tracer in
	// ClipFacesByPlaneAndCap. After welding the cut boundary, every cut-plane
	// edge sits on exactly one face (the side-wall fragment that the cut
	// produced) and is wound in that face's direction. The cap face winds the
	// opposite way, so we map b -> a for every (a, b) directed face edge whose
	// endpoints both lie on the cut plane, then chase the chain back to its
	// start to close each cap polygon. Returns one face per loop traced.
	private static List<FaceHandle> SynthesizeCapFaces( PolygonMesh mesh, Plane cutPlane, float epsilon )
	{
		var caps = new List<FaceHandle>();
		if ( mesh is null ) return caps;

		var onPlane = new HashSet<VertexHandle>();
		foreach ( var v in mesh.VertexHandles )
		{
			if ( MathF.Abs( cutPlane.GetDistance( mesh.GetVertexPosition( v ) ) ) <= epsilon )
				onPlane.Add( v );
		}
		if ( onPlane.Count < 3 ) return caps;

		var nextInCap = new Dictionary<VertexHandle, VertexHandle>();
		foreach ( var face in mesh.FaceHandles )
		{
			var verts = mesh.GetFaceVertices( face );
			for ( var i = 0; i < verts.Length; i++ )
			{
				var a = verts[i];
				var b = verts[(i + 1) % verts.Length];
				if ( !onPlane.Contains( a ) || !onPlane.Contains( b ) ) continue;
				nextInCap[b] = a;
			}
		}

		var visited = new HashSet<VertexHandle>();
		foreach ( var start in nextInCap.Keys.ToList() )
		{
			if ( visited.Contains( start ) ) continue;
			var loop = new List<VertexHandle>();
			var cur = start;
			var closed = false;
			while ( true )
			{
				if ( visited.Contains( cur ) ) break;
				visited.Add( cur );
				loop.Add( cur );
				if ( !nextInCap.TryGetValue( cur, out var next ) ) break;
				if ( next.Equals( start ) ) { closed = true; break; }
				cur = next;
				if ( loop.Count > nextInCap.Count ) break;
			}
			if ( !closed || loop.Count < 3 ) continue;

			var handles = loop.ToArray();
			var capFace = mesh.AddFace( handles );
			if ( capFace.IsValid ) caps.Add( capFace );
		}

		return caps;
	}

	private static void AlignCapFaces( PolygonMesh mesh, List<FaceHandle> capFaces, Rotation worldRotation )
	{
		if ( capFaces is null ) return;
		foreach ( var face in capFaces )
		{
			if ( !face.IsValid ) continue;
			mesh.ComputeFaceNormal( face, out var localNormal );
			var worldNormal = (worldRotation * localNormal).Normal;
			var (axisU, axisV) = ModelBrush.DefaultUVAxesForNormal( worldNormal );
			mesh.SetFaceTextureParameters( face,
				new Vector4( axisU, 0f ),
				new Vector4( axisV, 0f ),
				new Vector2( _capUVScale, _capUVScale ) );
		}
	}

	#endregion

	#region Toolbar Actions

	private void SetTool( Tool tool )
	{
		if ( IsPlayMode ) return;
		CurrentTool = tool;
	}

	private void SetSelectionMode( SelectionMode mode )
	{
		if ( IsPlayMode ) return;
		Selection = mode;
		CurrentTool = Tool.Move;
	}

	public void AddBoxAction() { ClearAllSelections(); SetTool( Tool.Box ); }
	public void AddSphereAction() { ClearAllSelections(); SetTool( Tool.Sphere ); }
	public void AddCylinderAction() { ClearAllSelections(); SetTool( Tool.Cylinder ); }

	public void ToggleLocalGizmoAction()
	{
		if ( IsPlayMode ) return;
		LocalGizmo = !LocalGizmo;
	}

	public void ToggleGridAction()
	{
		if ( IsPlayMode ) return;
		GridEnabled = !GridEnabled;
	}

	public void MirrorXAction() => MirrorAction( Vector3.Forward );
	public void MirrorYAction() => MirrorAction( Vector3.Left );
	public void MirrorZAction() => MirrorAction( Vector3.Up );

	// Mirror every selected, editable brush across the plane through the
	// selection's bounding-box centre whose normal is the world axis. Each
	// brush's mesh is rebuilt in local space (vertices reflected, face
	// winding reversed so normals still point outward) and its WorldPosition
	// is flipped across the same plane. Undo captures both the pre- and
	// post-mirror state via _undo.BeginEdit/EndEdit, and the new mesh is
	// pushed to peers through BroadcastMeshSnapshot — WorldPosition itself
	// is auto-synced by s&box.
	private void MirrorAction( Vector3 worldAxis )
	{
		if ( !CanMirror ) return;
		var targets = _selectedBrushes.Where( b => b.IsValid() && b.CanLocalEdit() ).ToList();
		if ( targets.Count == 0 ) return;
		if ( !TryGetSelectionCenter( out var center ) ) return;
		// Snap the mirror plane onto the grid for the same reason the move
		// and rotate gizmos do (see UpdateGizmo): the centroid of an
		// odd-sized or multi-brush selection rarely lands on a grid point,
		// and reflecting grid-aligned brushes across an off-grid plane
		// pulls them off the grid. No-op when the grid is disabled.
		AlignToGrid( ref center );

		_undo.BeginEdit( targets );

		var anyMirrored = false;
		foreach ( var brush in targets )
		{
			if ( TryMirrorBrush( brush, center, worldAxis ) ) anyMirrored = true;
		}

		if ( !anyMirrored )
		{
			_undo.CancelEdit();
			return;
		}

		_undo.EndEdit();
	}

	private bool TryMirrorBrush( ModelBrush brush, Vector3 worldCenter, Vector3 worldAxis )
	{
		var oldMesh = brush.PolygonMesh;
		if ( oldMesh is null ) return false;

		var oldTransform = brush.WorldTransform;

		// Mirror the brush's WorldPosition across the (worldCenter, worldAxis)
		// plane. Rotation and scale stay put — the reflection is baked into
		// the mesh instead (a det = -1 reflection can't be expressed as a
		// proper rotation).
		var rel = brush.WorldPosition - worldCenter;
		var newWorldPos = worldCenter + rel - 2f * Vector3.Dot( rel, worldAxis ) * worldAxis;
		var newTransform = new Transform( newWorldPos, brush.WorldRotation, brush.WorldScale );

		var newMesh = new PolygonMesh();
		var vertMap = new Dictionary<int, VertexHandle>();

		foreach ( var srcFace in oldMesh.FaceHandles )
		{
			var srcVerts = oldMesh.GetFaceVertices( srcFace );
			var n = srcVerts.Length;
			var newVerts = new VertexHandle[n];

			for ( var i = 0; i < n; i++ )
			{
				// Reverse winding so the new face's normal still points outward
				// after the reflection (mirror has det = -1, which flips face
				// orientation).
				var srcV = srcVerts[n - 1 - i];
				if ( !vertMap.TryGetValue( srcV.Index, out var newVert ) )
				{
					var worldOld = oldTransform.PointToWorld( oldMesh.GetVertexPosition( srcV ) );
					var relV = worldOld - worldCenter;
					var worldNew = worldCenter + relV - 2f * Vector3.Dot( relV, worldAxis ) * worldAxis;
					var localNew = newTransform.PointToLocal( worldNew );
					newVert = newMesh.AddVertex( localNew );
					vertMap[srcV.Index] = newVert;
				}
				newVerts[i] = newVert;
			}

			var newFace = newMesh.AddFace( newVerts );
			if ( !newFace.IsValid ) continue;

			var mat = oldMesh.GetFaceMaterial( srcFace );
			if ( mat != null ) newMesh.SetFaceMaterial( newFace, mat );

			oldMesh.GetFaceTextureParameters( srcFace, out var axisU, out var axisV, out var scale );
			newMesh.SetFaceTextureParameters( newFace, axisU, axisV, scale );
			newMesh.SetTextureScale( newFace, oldMesh.GetTextureScale( srcFace ) );
			newMesh.SetTextureOffset( newFace, oldMesh.GetTextureOffset( srcFace ) );
		}

		brush.WorldPosition = newWorldPos;
		brush.ReplacePolygonMesh( newMesh );
		BroadcastMeshSnapshot( brush, "mirror" );
		return true;
	}

	public void HideAction()
	{
		if ( !CanHide ) return;
		foreach ( var brush in _selectedBrushes )
		{
			if ( brush is null || !brush.IsValid() ) continue;
			_hiddenBrushes.Add( brush );
			var mc = brush.MeshComponent;
			if ( mc is not null ) mc.Enabled = false;
		}
		ClearAllSelections();
	}

	public void UnhideAllAction()
	{
		if ( IsPlayMode ) return;
		foreach ( var brush in _hiddenBrushes )
		{
			if ( brush is null || !brush.IsValid() ) continue;
			var mc = brush.MeshComponent;
			if ( mc is not null ) mc.Enabled = true;
		}
		_hiddenBrushes.Clear();
	}

	// Wipe every brush + cloud instance from the scene and snap the
	// player back to the world origin. Only the host can run this when
	// networked — otherwise peers would try to destroy networked objects
	// they don't own and end up in a partially-cleared state. Undo
	// history is dropped because every recorded slot now references
	// destroyed brushes.
	public void ResetSceneAction()
	{
		if ( IsPlayMode ) return;
		if ( Networking.IsActive && !Networking.IsHost ) return;

		if ( _placingCloud is not null ) CommitCloudPlacement();

		ClearAllSelections();
		_hiddenBrushes.Clear();
		_vertexContextBrushes.Clear();
		_gizmoActiveAxis = GizmoAxis.None;
		_gizmoHoveredAxis = GizmoAxis.None;
		_gizmoDragStartPositions.Clear();
		_gizmoDragStartPositionsCloud.Clear();
		_vertexDragStartLocal.Clear();
		_vertexDragAnchor = null;
		_gizmoShiftPendingClone = false;
		_rotActiveAxis = GizmoAxis.None;
		_rotHoveredAxis = GizmoAxis.None;
		_rotDragStartTransforms.Clear();
		_rotDragStartTransformsCloud.Clear();
		_rotDragBasisOverride = null;
		_scaleActiveSide = GizmoAxisSide.None;
		_scaleHoveredSide = GizmoAxisSide.None;
		_scaleDragStartTransforms.Clear();
		_scaleDragStartTransformsCloud.Clear();
		_sliceDragging = false;
		_sliceHasPick = false;
		_scalingUp = false;

		// Collect into a list before destroying — ModelBrush.OnDestroy
		// removes itself from the static registry, so iterating the live
		// HashSet while destroying would mutate-during-enumerate.
		var brushes = new List<ModelBrush>();
		var seen = new HashSet<ModelBrush>();
		foreach ( var b in Scene.GetAllComponents<ModelBrush>() )
		{
			if ( b.IsValid() && seen.Add( b ) ) brushes.Add( b );
		}
		foreach ( var b in ModelBrush.Brushes )
		{
			if ( b.IsValid() && seen.Add( b ) ) brushes.Add( b );
		}
		foreach ( var brush in brushes )
		{
			brush.GameObject?.Destroy();
		}

		foreach ( var ci in CloudInstance.All.ToList() )
		{
			if ( ci.IsValid() ) ci.GameObject?.Destroy();
		}

		if ( _playerController is not null )
		{
			_playerController.WorldPosition = Vector3.Zero;
		}

		_undo.Clear();
		_shLightVolume?.Clear();

		Selection = SelectionMode.Brush;
		CurrentTool = Tool.Move;
	}

	public void MoveAction() => SetTool( Tool.Move );
	public void RotateAction() { if ( !CanUseRotateScaleTool ) return; SetTool( Tool.Rotate ); }
	public void ScaleAction() { if ( !CanUseScaleTool ) return; SetTool( Tool.Scale ); }

	// UV-edit tools require at least one selected face (which implicitly
	// requires Face mode, since Selection setter clears the face set on
	// every mode change). Toolbar gates on CanUseUVTools.
	public bool CanUseUVTools => _selectedFaces.Count > 0;
	public void UVPanAction()    { if ( !CanUseUVTools ) return; SetTool( Tool.UVPan ); }
	public void UVScaleAction()  { if ( !CanUseUVTools ) return; SetTool( Tool.UVScale ); }
	public void UVRotateAction() { if ( !CanUseUVTools ) return; SetTool( Tool.UVRotate ); }

	public void ImportMapAction()
	{
		if ( IsPlayMode ) return;
		if ( !CanImport ) return;
		if ( _filePickerDialog is null ) return;
		var (initialDir, initialFile) = SplitFilePath( _lastMapFilename );
		_filePickerDialog.Show( "Import MAP", "*.map", initialDir, filename =>
		{
			string text;
			try
			{
				var bytes = FileSystem.Data.ReadAllBytes( filename ).ToArray();
				text = System.Text.Encoding.UTF8.GetString( bytes );
			}
			catch ( Exception e )
			{
				Log.Warning( $"togethercsg: failed to read map '{filename}': {e.Message}" );
				return;
			}
			_lastMapFilename = filename;
			ImportMapText( text, filename );
		}, null, true, initialFile );
	}

	public void ExportMapAction()
	{
		if ( IsPlayMode ) return;
		if ( _filePickerDialog is null ) return;
		var (initialDir, initialFile) = SplitFilePath( _lastMapFilename );
		var defaultFile = string.IsNullOrEmpty( initialFile ) ? "untitled.map" : initialFile;
		_filePickerDialog.ShowSave( "Export MAP", "*.map", ".map", defaultFile, initialDir, filename =>
		{
			string text;
			try
			{
				text = ExportMapText();
			}
			catch ( Exception e )
			{
				Log.Warning( $"togethercsg: failed to build map export: {e.Message}" );
				return;
			}
			try
			{
				FileSystem.Data.WriteAllBytes( filename, System.Text.Encoding.UTF8.GetBytes( text ) );
				_lastMapFilename = filename;
			}
			catch ( Exception e )
			{
				Log.Warning( $"togethercsg: failed to write map '{filename}': {e.Message}" );
			}
		}, null );
	}

	private void ImportMapText( string text, string sourceFilename )
	{
		var data = MapFile.Parse( text );
		if ( data.Entities.Count == 0 ) return;
		if ( _cubePrefab is null ) return;

		var baseFolder = GetParentFolder( sourceFilename );
		var textureCache = new Dictionary<string, string>( StringComparer.OrdinalIgnoreCase );

		// Drop selection state first so we don't keep stale handles to
		// brushes we're about to destroy, and so a selection-driven render
		// pass between Destroy() and end-of-frame doesn't try to outline a
		// half-destroyed brush.
		ClearAllSelections();
		_shLightVolume?.Clear();

		// Import replaces the whole scene: destroy every existing brush
		// inside the same undo transaction as the new ones we'll create, so
		// the entire import (clear + populate) collapses into a single
		// undo/redo entry. Collect from a live scene query so we don't miss
		// any brushes the static `ModelBrush.Brushes` registry might be
		// behind on, and union the registry in too as a belt-and-braces.
		var existing = new List<ModelBrush>();
		var seen = new HashSet<ModelBrush>();
		foreach ( var b in Scene.GetAllComponents<ModelBrush>() )
		{
			if ( b.IsValid() && b.CanLocalEdit() && seen.Add( b ) ) existing.Add( b );
		}
		foreach ( var b in ModelBrush.Brushes )
		{
			if ( b.IsValid() && b.CanLocalEdit() && seen.Add( b ) ) existing.Add( b );
		}

		// Same story for existing cloud / map entities — sweep them so the
		// imported scene is the only one left standing.
		var existingClouds = CloudInstance.All
			.Where( c => c.IsValid() && c.GameObject is not null && c.GameObject.Enabled )
			.ToList();

		_undo.BeginEdit( existing );
		try
		{
			foreach ( var brush in existing )
			{
				brush.GameObject?.Destroy();
				_undo.MarkBrushDestroyed( brush );
			}
			foreach ( var ci in existingClouds )
			{
				_undo.TrackExistingCloud( ci );
				if ( Networking.IsActive )
				{
					ci.RpcSetActive( false );
				}
				else if ( ci.GameObject is not null )
				{
					ci.GameObject.Enabled = false;
				}
			}

			foreach ( var entity in data.Entities )
			{
				foreach ( var brush in entity.Brushes )
				{
					if ( !MapFile.TryBuildPolygonMesh( brush, out var mesh, out var center ) ) continue;

					ApplyMapFaceMaterials( mesh, brush, center, baseFolder, textureCache );

					var go = _cubePrefab.Clone();
					go.WorldPosition = center;
					go.WorldRotation = Rotation.Identity;
					go.LocalScale = Vector3.One;
					if ( Networking.IsActive ) go.NetworkSpawn();

					var modelBrush = go.GetComponent<ModelBrush>();
					if ( modelBrush is null )
					{
						go.Destroy();
						continue;
					}

					modelBrush.ReplacePolygonMesh( mesh );
					BroadcastMeshSnapshot( modelBrush, "import map" );
					_undo.TrackNewBrush( modelBrush );
				}

				// Worldspawn carries the brushes only — non-worldspawn entities
				// (info_player_start, light, etc.) become editor-side
				// placeholders so they can be moved around and round-tripped
				// back out on the next export.
				if ( !string.Equals( entity.ClassName, "worldspawn", StringComparison.OrdinalIgnoreCase )
					&& entity.Brushes.Count == 0 )
				{
					SpawnEntityFromMap( entity );
				}
			}
		}
		finally
		{
			_undo.EndEdit();
		}

		// Drop into the brush+move "neutral selection" combo with nothing
		// selected, mirroring what clicking the BRUSH toolbar button does
		// from a clean slate. Clear again here in case anything got added to
		// a selection set during brush construction.
		ClearAllSelections();
		Selection = SelectionMode.Brush;
		CurrentTool = Tool.Move;
	}

	// Spawn an editor placeholder for one parsed MAP entity. If the entity
	// carries an `sbox_workshop` property we kick off an async cloud fetch
	// for that ident — when it lands we replace the temporary box placeholder
	// with the matching cloud asset. The temporary placeholder is what the
	// undo system tracks so the import is reversible even if the async
	// fetch hasn't completed yet.
	private void SpawnEntityFromMap( MapFile.Entity entity )
	{
		var transform = TransformFromMapEntity( entity );
		var ci = SpawnMapEntityPlaceholder( entity.ClassName, entity.Properties, transform );
		if ( ci is null ) return;
		_undo.TrackNewCloud( ci );

		if ( entity.Properties.TryGetValue( "sbox_workshop", out var ident ) && !string.IsNullOrEmpty( ident ) )
		{
			_ = ReplaceWithCloudAsset( ci, ident );
		}
	}

	private async Task ReplaceWithCloudAsset( CloudInstance placeholder, string ident )
	{
		Package package;
		try
		{
			package = await Package.FetchAsync( ident, false );
		}
		catch ( Exception e )
		{
			Log.Warning( $"togethercsg: failed to fetch cloud package '{ident}': {e.Message}" );
			return;
		}
		if ( package is null ) return;
		if ( placeholder is null || !placeholder.IsValid() ) return;

		// Preserve the placeholder's transform + property bag so the cloud
		// replacement reads back identically on a subsequent export.
		var transform = placeholder.WorldTransform;
		var properties = placeholder.EnumerateProperties().ToList();
		var className = placeholder.ClassName;

		placeholder.GameObject?.Destroy();

		// SpawnCloudModel/Entity each open placement mode and replace the
		// active selection with the new placeholder. We're driving them from
		// an async import callback, not a user action, so snapshot the
		// selection state and put it back afterwards.
		var savedSelection = Selection;
		var savedTool = CurrentTool;

		CloudInstance replacement = null;
		switch ( package.TypeName )
		{
			case "model":
				await SpawnCloudModel( package, transform );
				break;
			case "sent":
			case "entity":
				await SpawnCloudEntityPlaceholder( package, transform );
				break;
			default:
				Log.Warning( $"togethercsg: don't know how to spawn cloud package type '{package.TypeName}'" );
				return;
		}

		if ( _placingCloud is not null )
		{
			replacement = _placingCloud;
			CommitCloudPlacement();
		}

		// Restore the user's editor state — BeginCloudPlacement clobbered
		// selection / tool to drive interactive placement.
		ClearAllSelections();
		Selection = savedSelection;
		CurrentTool = savedTool;

		if ( replacement is null || !replacement.IsValid() ) return;

		replacement.WorldTransform = transform;
		if ( !string.IsNullOrEmpty( className ) ) replacement.ClassName = className;
		replacement.SetProperties( properties );
	}

	// Extract `origin "x y z"` and `angle/angles` from an entity's properties
	// into a world transform. Missing fields default to the world origin and
	// identity rotation. `angle` is the Quake yaw convention (degrees around
	// world Z, 0 = +X, 90 = +Y).
	private static Transform TransformFromMapEntity( MapFile.Entity entity )
	{
		var position = Vector3.Zero;
		if ( entity.Properties.TryGetValue( "origin", out var originStr ) )
		{
			TryParseVector3( originStr, out position );
		}

		var rotation = Rotation.Identity;
		if ( entity.Properties.TryGetValue( "angles", out var anglesStr )
			&& TryParseVector3( anglesStr, out var angles ) )
		{
			// Quake `angles` is pitch yaw roll (degrees).
			rotation = Rotation.From( angles.x, angles.y, angles.z );
		}
		else if ( entity.Properties.TryGetValue( "angle", out var angleStr )
			&& float.TryParse( angleStr, System.Globalization.NumberStyles.Float,
				System.Globalization.CultureInfo.InvariantCulture, out var yaw ) )
		{
			rotation = Rotation.FromYaw( yaw );
		}

		return new Transform( position, rotation, 1f );
	}

	private static bool TryParseVector3( string s, out Vector3 v )
	{
		v = Vector3.Zero;
		if ( string.IsNullOrEmpty( s ) ) return false;
		var parts = s.Split( ' ', System.StringSplitOptions.RemoveEmptyEntries | System.StringSplitOptions.TrimEntries );
		if ( parts.Length < 3 ) return false;
		var culture = System.Globalization.CultureInfo.InvariantCulture;
		var style = System.Globalization.NumberStyles.Float;
		if ( !float.TryParse( parts[0], style, culture, out var x ) ) return false;
		if ( !float.TryParse( parts[1], style, culture, out var y ) ) return false;
		if ( !float.TryParse( parts[2], style, culture, out var z ) ) return false;
		v = new Vector3( x, y, z );
		return true;
	}

	// Resolve and apply per-face materials directly on the freshly-built mesh
	// before handing it to ReplacePolygonMesh. Each MAP texture name is mapped
	// to a PNG on disk (cached so we only read each one once), loaded through
	// GameNetwork.RegisterLocalTexture so other clients receive the bytes via
	// the existing texture broadcast, then assigned to its face. After loading
	// we know the real texture size, so we recompute the face's UV parameters
	// using MapFile's Quake/Valve formula. The mesh snapshot we broadcast
	// after ReplacePolygonMesh carries the material assignments to remote
	// peers.
	private void ApplyMapFaceMaterials(
		PolygonMesh mesh,
		MapFile.Brush mapBrush,
		Vector3 center,
		string baseFolder,
		Dictionary<string, string> textureCache )
	{
		if ( mesh is null ) return;
		var faces = mesh.FaceHandles.ToArray();
		var count = Math.Min( faces.Length, mapBrush.Faces.Count );
		for ( var i = 0; i < count; i++ )
		{
			var mapFace = mapBrush.Faces[i];
			var texName = mapFace.Texture;
			if ( string.IsNullOrEmpty( texName ) ) continue;
			if ( texName.StartsWith( "__", StringComparison.Ordinal ) ) continue;
			var path = ResolveMapTexturePath( texName, baseFolder, textureCache );
			if ( string.IsNullOrEmpty( path ) ) continue;
			if ( !EnsureMapMaterial( path, out var material ) ) continue;
			mesh.SetFaceMaterial( faces[i], material );

			var (texW, texH) = GetMapTextureSize( path );
			MapFile.ApplyFaceTextureParameters( mesh, faces[i], mapFace, center, texW, texH );
		}
	}

	private static (float w, float h) GetMapTextureSize( string path )
	{
		float w = 0f, h = 0f;
		if ( GameNetwork.TryGetTexture( path, out var tex ) && tex is not null )
		{
			w = tex.Width;
			h = tex.Height;
		}
		return MapFile.DefaultTextureSize( w, h );
	}

	private string ResolveMapTexturePath( string textureName, string baseFolder, Dictionary<string, string> cache )
	{
		if ( cache.TryGetValue( textureName, out var cached ) ) return cached;
		string Found( string p ) { cache[textureName] = p; return p; }
		// Try the literal name first (in case the MAP carries an extension or a
		// path relative to /data), then `${name}.png` next to the .map file, and
		// finally `${name}.png` at the data root.
		if ( FileSystem.Data.FileExists( textureName ) ) return Found( textureName );
		var withExt = textureName.IndexOf( '.' ) >= 0 ? textureName : textureName + ".png";
		if ( !string.IsNullOrEmpty( baseFolder ) )
		{
			var inBase = $"{baseFolder}/{withExt}";
			if ( FileSystem.Data.FileExists( inBase ) ) return Found( inBase );
		}
		if ( FileSystem.Data.FileExists( withExt ) ) return Found( withExt );
		cache[textureName] = null;
		return null;
	}

	private bool EnsureMapMaterial( string path, out Material material )
	{
		material = null;
		if ( GameNetwork.TryGetMaterial( path, out material ) ) return true;
		byte[] data;
		try
		{
			data = FileSystem.Data.ReadAllBytes( path ).ToArray();
		}
		catch ( Exception e )
		{
			Log.Warning( $"togethercsg: failed to load map texture '{path}': {e.Message}" );
			return false;
		}
		GameNetwork.RegisterLocalTexture( path, data, _templateMaterial );
		return GameNetwork.TryGetMaterial( path, out material );
	}

	private static string GetParentFolder( string filename )
	{
		if ( string.IsNullOrEmpty( filename ) ) return "";
		var idx = filename.LastIndexOf( '/' );
		return idx < 0 ? "" : filename.Substring( 0, idx );
	}

	// Split a "dir/sub/file.ext" path into ("dir/sub", "file.ext"). Empty
	// string yields ("", ""); a bare name with no slash yields ("", name).
	// Used to seed the FilePickerDialog's path + preselected file from a
	// single stored full-path string.
	private static (string Directory, string File) SplitFilePath( string fullPath )
	{
		if ( string.IsNullOrEmpty( fullPath ) ) return ("", "");
		var normalised = fullPath.Replace( '\\', '/' );
		var slash = normalised.LastIndexOf( '/' );
		if ( slash < 0 ) return ("", normalised);
		return (normalised.Substring( 0, slash ), normalised.Substring( slash + 1 ));
	}

	private string ExportMapText()
	{
		var brushes = new List<MapFile.Brush>();
		foreach ( var modelBrush in ModelBrush.Brushes )
		{
			if ( !modelBrush.IsValid() ) continue;
			var mapBrush = MapFile.BuildBrushFromModel( modelBrush );
			if ( mapBrush is not null && mapBrush.Faces.Count >= 4 ) brushes.Add( mapBrush );
		}

		// Each editor-side CloudInstance becomes a MAP entity block. Position
		// and rotation are written as `origin` / `angles`; cloud-backed ones
		// also carry an `sbox_workshop` ident so a re-import can recover the
		// original asset (see ReplaceWithCloudAsset on the import side).
		var entities = new List<MapFile.Entity>();
		foreach ( var ci in CloudInstance.All )
		{
			if ( !ci.IsValid() ) continue;
			if ( ci.GameObject is null || !ci.GameObject.Enabled ) continue;

			var entity = new MapFile.Entity();

			var className = ci.ClassName;
			if ( string.IsNullOrEmpty( className ) )
			{
				className = ci.Kind switch
				{
					CloudInstance.AssetKind.Model => "sbox_workshop_model",
					CloudInstance.AssetKind.Entity => "sbox_workshop_entity",
					_ => "info_null",
				};
			}
			entity.ClassName = className;

			// Round-trip every property the entity carried on import first,
			// then overlay the transform-derived fields so a moved entity
			// writes its current world position even if the loaded `origin`
			// is now stale.
			foreach ( var kv in ci.EnumerateProperties() )
			{
				entity.Properties[kv.Key] = kv.Value;
			}

			entity.Properties["origin"] = FormatVector3( ci.WorldPosition );
			var angles = ci.WorldRotation.Angles();
			// Stash the full pitch/yaw/roll so we keep arbitrary orientations
			// (the gizmo can produce any rotation). `angle` stays as a yaw-only
			// alias for tools that only read that field.
			entity.Properties["angles"] = FormatVector3( new Vector3( angles.pitch, angles.yaw, angles.roll ) );
			entity.Properties["angle"] = angles.yaw.ToString( "0.######", System.Globalization.CultureInfo.InvariantCulture );

			if ( !string.IsNullOrEmpty( ci.PackageIdent )
				&& (ci.Kind == CloudInstance.AssetKind.Entity || ci.Kind == CloudInstance.AssetKind.Model) )
			{
				entity.Properties["sbox_workshop"] = ci.PackageIdent;
			}

			entities.Add( entity );
		}

		return MapFile.WriteMap( brushes, entities );
	}

	private static string FormatVector3( Vector3 v )
	{
		var c = System.Globalization.CultureInfo.InvariantCulture;
		return $"{v.x.ToString( "0.######", c )} {v.y.ToString( "0.######", c )} {v.z.ToString( "0.######", c )}";
	}

	public void CloneAction()
	{
		if ( IsPlayMode || !CanCloneOrDelete ) return;
		var step = _gridSize > 0f ? _gridSize : _fallbackGridStep;
		_undo.BeginEdit();
		CloneSelection( new Vector3( step, step, step ) );
		_undo.EndEdit();
	}

	public void CloneSelectionInPlace()
	{
		CloneSelection( Vector3.Zero );
	}

	private void CloneSelection( Vector3 offset )
	{
		var sources = CollectSelectedBrushes().ToList();
		var cloudSources = _selectedCloud.Where( c => c.IsValid() ).ToList();
		if ( sources.Count == 0 && cloudSources.Count == 0 ) return;

		// Capture lock state once so every clone (and every peer receiving
		// the RPC) decides on the same UV semantics — otherwise different
		// peers' Player.LockUVs could diverge the result mid-broadcast.
		var lockUVs = LockUVs;

		var clones = new List<ModelBrush>( sources.Count );
		foreach ( var source in sources )
		{
			if ( !source.IsValid() ) continue;

			var cloneObject = new GameObject();
			cloneObject.Name = source.GameObject.Name;
			cloneObject.WorldRotation = source.WorldRotation;
			cloneObject.WorldScale = source.WorldScale;
			cloneObject.WorldPosition = source.WorldPosition + offset;

			var cloneBrush = cloneObject.Components.Create<ModelBrush>();
			cloneBrush.ModelMaterial = source.ModelMaterial;
			cloneBrush.CopyMeshFrom( source, lockUVs );

			if ( Networking.IsActive )
			{
				cloneObject.NetworkSpawn();
				cloneBrush.RpcCopyMeshFrom( source, lockUVs );
			}

			clones.Add( cloneBrush );
			_undo.TrackNewBrush( cloneBrush );
		}

		var cloudClones = new List<CloudInstance>( cloudSources.Count );
		foreach ( var source in cloudSources )
		{
			var cloneObject = source.GameObject.Clone();
			cloneObject.WorldRotation = source.WorldRotation;
			cloneObject.WorldScale = source.WorldScale;
			cloneObject.WorldPosition = source.WorldPosition + offset;

			var cloneCi = cloneObject.GetComponent<CloudInstance>();
			if ( cloneCi is null ) cloneCi = cloneObject.AddComponent<CloudInstance>();
			cloneCi.Kind = source.Kind;
			cloneCi.PackageIdent = source.PackageIdent;
			cloneCi.ThumbUrl = source.ThumbUrl;
			cloneCi.ClassName = source.ClassName;
			cloneCi.SetProperties( source.EnumerateProperties() );
			cloneCi.Renderer = cloneObject.GetComponent<ModelRenderer>();

			if ( Networking.IsActive ) cloneObject.NetworkSpawn( true, null );
			cloudClones.Add( cloneCi );
			_undo.TrackNewCloud( cloneCi );
		}

		if ( clones.Count == 0 && cloudClones.Count == 0 ) return;

		Selection = cloudClones.Count > 0 ? SelectionMode.Entity : SelectionMode.Brush;
		_selectedBrushes.Clear();
		foreach ( var clone in clones )
		{
			_selectedBrushes.Add( clone );
		}
		_selectedCloud.Clear();
		foreach ( var clone in cloudClones )
		{
			_selectedCloud.Add( clone );
		}
	}

	public void DeleteAction()
	{
		if ( IsPlayMode || !CanCloneOrDelete ) return;
		var targets = CollectSelectedBrushes().ToList();
		var cloudTargets = _selectedCloud.Where( c => c.IsValid() ).ToList();
		if ( targets.Count == 0 && cloudTargets.Count == 0 ) return;

		_undo.BeginEdit( targets );
		foreach ( var ci in cloudTargets )
		{
			_undo.TrackExistingCloud( ci );
		}

		foreach ( var brush in targets )
		{
			if ( !brush.IsValid() ) continue;
			if ( !brush.CanLocalEdit() ) continue;
			brush.GameObject?.Destroy();
			_undo.MarkBrushDestroyed( brush );
		}

		foreach ( var ci in cloudTargets )
		{
			if ( !ci.IsValid() || ci.GameObject is null ) continue;
			if ( Networking.IsActive )
			{
				ci.RpcSetActive( false );
			}
			else
			{
				ci.GameObject.Enabled = false;
			}
		}

		ClearAllSelections();
		_undo.EndEdit();
	}

	public void SliceAction()
	{
		if ( IsPlayMode ) return;
		if ( !CanSlice ) return;
		CurrentTool = Tool.Slice;
		_sliceDragging = false;
		_sliceHasPick = false;
	}

	public void BrushAction()
	{
		if ( IsPlayMode ) return;
		// Switching out of Vertex / Face / Edge mode wipes the sub-element
		// selection, but the user was already working on those brushes —
		// promote them into _selectedBrushes so they stay selected in Brush
		// mode without the user having to re-click each one. Other entry
		// paths (e.g. starting from Brush or Entity mode) just clear, since
		// there's no sub-element context to carry forward.
		var carryOver = new HashSet<ModelBrush>();
		switch ( Selection )
		{
			case SelectionMode.Vertex:
				foreach ( var b in _vertexContextBrushes )
				{
					if ( b is not null && b.IsValid() ) carryOver.Add( b );
				}
				break;
			case SelectionMode.Face:
				foreach ( var (b, _) in _selectedFaces )
				{
					if ( b is not null && b.IsValid() ) carryOver.Add( b );
				}
				break;
			case SelectionMode.Edge:
				foreach ( var (b, _, _) in _selectedEdges )
				{
					if ( b is not null && b.IsValid() ) carryOver.Add( b );
				}
				break;
		}
		SetSelectionMode( SelectionMode.Brush );
		foreach ( var b in carryOver ) _selectedBrushes.Add( b );
	}
	public void EntityAction() => SetSelectionMode( SelectionMode.Entity );

	public void VertexAction()
	{
		if ( IsPlayMode ) return;
		// Without a brush context Vertex mode picks nothing — keep the user
		// out instead of dropping them into a no-op mode. Also catches the
		// keyboard shortcut path so pressing 3 with nothing selected is a
		// no-op, matching the toolbar button being greyed out.
		if ( !CanUseVertexMode ) return;
		// Capture the currently-selected brushes BEFORE the Selection
		// setter clears _selectedBrushes. These become the "context
		// brushes" for vertex picking — only their vertices are pickable
		// by clicking.
		var context = _selectedBrushes.Where( b => b.IsValid() ).ToList();
		Selection = SelectionMode.Vertex;
		CurrentTool = Tool.Move;
		foreach ( var b in context ) _vertexContextBrushes.Add( b );
	}

	public void FaceAction()
	{
		if ( IsPlayMode ) return;
		var previouslySelected = _selectedBrushes.ToList();
		Selection = SelectionMode.Face;
		CurrentTool = Tool.Move;
		foreach ( var brush in previouslySelected )
		{
			if ( !brush.IsValid() ) continue;
			var triangleCount = brush.Model.GetIndices().Length / 3;
			var seenFaces = new HashSet<int>();
			for ( var t = 0; t < triangleCount; t++ )
			{
				var faceIndex = brush.PolygonMesh.TriangleToFace( t ).Index;
				if ( seenFaces.Add( faceIndex ) )
				{
					_selectedFaces.Add( (brush, t) );
				}
			}
		}
		RefreshSelectedTexturePath();
	}

	// UNION and INTERSECTION are disabled: the CSG export format only handles
	// convex brushes and those operators can leave concave results behind.
	// public void UnionAction() { if ( IsPlayMode ) return; RunBooleanOperation( PolygonMesh.BooleanOperation.Union ); }
	// public void IntersectionAction() { if ( IsPlayMode ) return; RunBooleanOperation( PolygonMesh.BooleanOperation.Intersect ); }

	// SUBTRACTION carves every selected brush out of each other brush whose
	// world AABB it overlaps. Sources stay put (destroySource: false);
	// hidden brushes and brushes the local peer can't edit are skipped.
	//
	// Candidates are re-collected for each source so that fragments
	// spawned by an earlier source's subtract get carved by every
	// remaining source. Without that, the brush-CSG path keeps only the
	// first convex piece on the original target — later cutters acting
	// on that single piece silently no-op when they sit outside it, and
	// the user sees "only the first cutter actually did anything".
	//
	// Selected brushes are not excluded from being candidates, so a
	// group of overlapping selected brushes mutually carve each other.
	// (Order-dependent for the source-modified-as-target case, but the
	// visible result is now "every selected brush lost the overlapping
	// volume" instead of "nothing happened".)
	public void SubtractionAction()
	{
		if ( IsPlayMode ) return;
		if ( _selectedBrushes.Count < 1 ) return;

		var sources = _selectedBrushes.Where( b => b.IsValid() && b.CanLocalEdit() ).ToList();
		if ( sources.Count == 0 ) return;

		var sourceBounds = sources.ToDictionary( b => b, b => ComputeWorldAabb( b.Bounds, b.WorldTransform ) );

		// Snapshot the brushes that already existed when the transaction
		// opened. Anything that appears in ModelBrush.Brushes after a
		// RpcApplyBoolean is a fragment spawned by the brush-CSG path —
		// we have to TrackNewBrush each one so Undo destroys them along
		// with restoring the carved targets. Otherwise undo leaves an
		// uncarved target plus a pile of orphan fragment pieces.
		var preExisting = new HashSet<ModelBrush>( ModelBrush.Brushes );

		var touched = new HashSet<ModelBrush>();
		_undo.BeginEdit();
		foreach ( var source in sources )
		{
			if ( !source.IsValid() ) continue;
			var sBounds = sourceBounds[source];

			// Snapshot the live brush set so fragments spawned inside
			// the RpcApplyBoolean calls below don't blow up this
			// foreach. Fragments from an earlier source are visible to
			// later sources because the snapshot is re-taken each
			// outer iteration.
			var candidates = ModelBrush.Brushes.ToList();
			foreach ( var candidate in candidates )
			{
				if ( !candidate.IsValid() ) continue;
				if ( candidate == source ) continue;
				if ( _hiddenBrushes.Contains( candidate ) ) continue;
				if ( !candidate.CanLocalEdit() ) continue;

				var targetBounds = ComputeWorldAabb( candidate.Bounds, candidate.WorldTransform );
				if ( !AabbOverlaps( targetBounds, sBounds ) ) continue;

				_undo.TrackExisting( candidate );
				touched.Add( candidate );
				candidate.RpcApplyBoolean( source, PolygonMesh.BooleanOperation.Subtract, destroySource: false );

				// Capture fragments spawned by THIS subtract before the
				// next outer iteration can reach them. TrackNewBrush
				// stores Before=Exists=false; any later TrackExisting on
				// the same slot is a no-op, so order matters — if a
				// fragment first shows up via TrackExisting (as a
				// candidate for a later source), its Before would be
				// Exists=true and undo wouldn't destroy it.
				foreach ( var brush in ModelBrush.Brushes )
				{
					if ( !brush.IsValid() ) continue;
					if ( !preExisting.Add( brush ) ) continue;
					_undo.TrackNewBrush( brush );
					touched.Add( brush );
				}
			}
		}

		if ( touched.Count == 0 )
		{
			_undo.CancelEdit();
			return;
		}
		_undo.EndEdit();
	}

	private static BBox ComputeWorldAabb( BBox local, Transform transform )
	{
		var min = new Vector3( float.PositiveInfinity, float.PositiveInfinity, float.PositiveInfinity );
		var max = new Vector3( float.NegativeInfinity, float.NegativeInfinity, float.NegativeInfinity );
		ExpandWorldBounds( local, transform, ref min, ref max );
		return new BBox( min, max );
	}

	private static bool AabbOverlaps( BBox a, BBox b )
	{
		return a.Mins.x <= b.Maxs.x && a.Maxs.x >= b.Mins.x
			&& a.Mins.y <= b.Maxs.y && a.Maxs.y >= b.Mins.y
			&& a.Mins.z <= b.Maxs.z && a.Maxs.z >= b.Mins.z;
	}

	#endregion

	#region Texture

	public void SelectTextureAction()
	{
		if ( IsPlayMode ) return;
		if ( _selectedFaces.Count > 0 )
		{
			var brushes = _selectedFaces.Select( f => f.Item1 ).Distinct().ToList();
			if ( brushes.Any( b => !b.CanLocalEdit() ) )
			{
				return;
			}
		}
		if ( Selection == SelectionMode.Brush && _selectedBrushes.Count > 0 )
		{
			if ( _selectedBrushes.Any( b => !b.CanLocalEdit() ) ) return;
		}
		var (texDir, texFile) = SplitFilePath( SelectedTexturePath );
		_filePickerDialog.Show( "Select Texture", "*.png;", texDir, delegate ( string filename )
			{
				if ( !GameNetwork.TryGetMaterial( filename, out _ ) )
				{
					byte[] data;
					try
					{
						data = FileSystem.Data.ReadAllBytes( filename ).ToArray();
					}
					catch
					{
						return;
					}
					GameNetwork.RegisterLocalTexture( filename, data, _templateMaterial );
				}
				SelectedTexturePath = filename;
				ApplyTextureToSelection( filename );
			}, null, true, texFile );
	}

	// Push the given texture path to every face / brush in the current
	// selection (faces in Face mode, every face of every selected brush in
	// Brush mode). Used by both the texture-picker file dialog AND by the
	// PICK tool when the user clicks while having a selection. Wraps the
	// mutation in undo and respects per-brush ownership; CanLocalEdit gates
	// out peer-owned brushes.
	private void ApplyTextureToSelection( string path )
	{
		if ( string.IsNullOrEmpty( path ) ) return;

		var touched = new HashSet<ModelBrush>();
		foreach ( var (brush, _) in _selectedFaces ) touched.Add( brush );
		if ( Selection == SelectionMode.Brush )
		{
			foreach ( var brush in _selectedBrushes ) touched.Add( brush );
		}
		if ( touched.Count == 0 ) return;

		_undo.BeginEdit( touched );
		foreach ( var (brush, triangle) in _selectedFaces )
		{
			if ( !brush.CanLocalEdit() ) continue;
			brush.RpcSetFaceMaterial( triangle, path );
		}
		if ( Selection == SelectionMode.Brush )
		{
			foreach ( var brush in _selectedBrushes )
			{
				if ( !brush.CanLocalEdit() ) continue;
				brush.RpcSetAllFaceMaterial( path );
			}
		}
		_undo.EndEdit();
	}

	public void TogglePickTextureAction()
	{
		if ( IsPlayMode ) return;
		// Toggling PICK off returns to the default Move tool. SetTool would
		// be the obvious choice but Tool.Pick isn't a SetTool case anywhere
		// — assigning CurrentTool directly is enough since other tool
		// buttons reassign it the same way.
		CurrentTool = CurrentTool == Tool.Pick ? Tool.Move : Tool.Pick;
	}

	// Active while the PICK tool is on. Each frame: trace under the cursor,
	// draw a face outline on the hit brush, and on click copy that face's
	// material path into SelectedTexturePath. The texture preview in the
	// right panel updates automatically via SelectedTexturePath's setter.
	private void UpdateTexturePicking( Ray ray )
	{
		var traceResult = Scene.SceneWorld.Trace.Ray( ray, MaxRayDistance ).WithoutTags( "player", SelectionIgnoreTag ).Run();
		if ( !traceResult.Hit ) return;
		var brush = traceResult.SceneObject?.GetGameObject()?.GetComponent<ModelBrush>();
		if ( brush is null || brush.PolygonMesh is null ) return;

		brush.HoverFace( traceResult.HitTriangle );

		if ( !Input.Pressed( "attack1" ) ) return;

		var face = brush.PolygonMesh.TriangleToFace( traceResult.HitTriangle );
		var material = brush.PolygonMesh.GetFaceMaterial( face );
		var path = GameNetwork.GetPathForMaterial( material );
		// Untextured face: leave the tool active so the user can try
		// another face rather than silently exiting with nothing picked.
		if ( string.IsNullOrEmpty( path ) ) return;
		SelectedTexturePath = path;

		// Push the picked texture to any current selection (no-op when
		// nothing is selected) and drop back to the Move tool. Keeping the
		// selection intact so the user can keep editing the same set; only
		// the tool changes.
		ApplyTextureToSelection( path );
		CurrentTool = Tool.Move;
	}

	private void RefreshSelectedTexturePath()
	{
		if ( _selectedFaces.Count == 0 ) return;
		foreach ( var (brush, triangle) in _selectedFaces )
		{
			var face = brush.PolygonMesh.TriangleToFace( triangle );
			var material = brush.PolygonMesh.GetFaceMaterial( face );
			var path = GameNetwork.GetPathForMaterial( material );
			if ( !string.IsNullOrEmpty( path ) )
			{
				SelectedTexturePath = path;
				return;
			}
		}
	}

	#endregion

	#region UV Editing

	private static void PublishUVSnapshotToPeers( IEnumerable<ModelBrush> brushes )
	{
		foreach ( var brush in brushes )
		{
			if ( !brush.IsValid() ) continue;
			if ( !brush.CanLocalEdit() ) continue;
			byte[] data;
			try
			{
				data = brush.SerializeMeshState();
			}
			catch ( Exception e )
			{
				Log.Warning( $"togethercsg: drag-end UV snapshot serialise failed: {e.Message}" );
				continue;
			}
			using ( Rpc.FilterExclude( Connection.Local ) )
			{
				brush.RpcApplyMeshSnapshot( data );
			}
		}
	}

	public void IncreaseAngleSnap()
	{
		var i = Array.IndexOf( AngleSnapValues, _angleSnap );
		if ( i < 0 ) i = 4;
		_angleSnap = AngleSnapValues[Math.Min( i + 1, AngleSnapValues.Length - 1 )];
	}

	public void DecreaseAngleSnap()
	{
		var i = Array.IndexOf( AngleSnapValues, _angleSnap );
		if ( i < 0 ) i = 4;
		_angleSnap = AngleSnapValues[Math.Max( i - 1, 0 )];
	}

	private float SnapAngle( float deg )
	{
		if ( !_gridEnabled ) return deg;
		var snap = _angleSnap;
		if ( snap <= 0f ) return deg;
		return MathF.Round( deg / snap ) * snap;
	}

	private float ReadUVAgreement( Func<PolygonMesh, FaceHandle, float> sample )
	{
		if ( _selectedFaces.Count == 0 )
		{
			return float.NaN;
		}
		float? value = null;
		foreach ( var (brush, triangle) in _selectedFaces )
		{
			var face = brush.PolygonMesh.TriangleToFace( triangle );
			var sampled = sample( brush.PolygonMesh, face );
			if ( value is null )
			{
				value = sampled;
				continue;
			}
			if ( value.Value != sampled )
			{
				return float.NaN;
			}
		}
		return value ?? float.NaN;
	}

	private void WriteUVPerFace( Action<PolygonMesh, FaceHandle> mutate )
	{
		var touched = _selectedFaces.Select( f => f.Item1 ).Where( b => b is not null ).Distinct().ToList();
		if ( touched.Count > 0 ) _undo.BeginEdit( touched );
		foreach ( var (brush, triangle) in _selectedFaces )
		{
			if ( !brush.CanLocalEdit() ) continue;
			var face = brush.PolygonMesh.TriangleToFace( triangle );
			mutate( brush.PolygonMesh, face );
			var scale = brush.PolygonMesh.GetTextureScale( face );
			var offset = brush.PolygonMesh.GetTextureOffset( face );
			brush.RpcSetFaceUVScaleOffset( triangle, scale, offset );
		}
		ApplyUV();
		if ( touched.Count > 0 ) _undo.EndEdit();
	}

	private Dictionary<ModelBrush, HashSet<FaceHandle>> GroupSelectedFacesByBrush()
	{
		var brushFaces = new Dictionary<ModelBrush, HashSet<FaceHandle>>();
		foreach ( var (brush, triangle) in _selectedFaces )
		{
			if ( brush is null ) continue;
			if ( !brushFaces.TryGetValue( brush, out var set ) )
			{
				set = new HashSet<FaceHandle>();
				brushFaces[brush] = set;
			}
			set.Add( brush.PolygonMesh.TriangleToFace( triangle ) );
		}
		return brushFaces;
	}

	private static void RebuildBrushMeshComponent( ModelBrush brush )
	{
		brush.MeshComponent.Enabled = false;
		brush.MeshComponent.RebuildMesh();
		brush.MeshComponent.Enabled = true;
	}

	public void ApplyUV()
	{
		if ( _selectedFaces.Count == 0 ) return;

		foreach ( var (brush, faces) in GroupSelectedFacesByBrush() )
		{
			brush.PolygonMesh.ComputeFaceTextureCoordinatesFromParameters( faces );
			RebuildBrushMeshComponent( brush );
		}
	}

	// UV-tool dispatch. While in Tool.UVPan / .UVScale / .UVRotate AND
	// Face mode AND with at least one selected face, LMB-press begins a
	// per-face snapshot, each tick applies the op against the snapshot,
	// and LMB-release commits + RPCs the final params. Always returns
	// after handling so the existing selection / hover paths are skipped
	// for these tools.
	private void UpdateUVTool( Ray ray )
	{
		var kind = ToolToUVOpKind( CurrentTool );
		if ( kind == UVOpKind.None )
		{
			if ( _uvOpActive != UVOpKind.None ) EndUVOp( commit: true );
			return;
		}
		if ( Selection != SelectionMode.Face || _selectedFaces.Count == 0 )
		{
			if ( _uvOpActive != UVOpKind.None ) EndUVOp( commit: true );
			return;
		}

		if ( Input.Pressed( "attack1" ) )
		{
			BeginUVOp( ray, kind );
		}

		if ( _uvOpActive == UVOpKind.None ) return;

		if ( !Input.Down( "attack1" ) )
		{
			EndUVOp( commit: true );
			return;
		}

		ApplyUVOp( ray );
	}

	private static UVOpKind ToolToUVOpKind( Tool tool ) => tool switch
	{
		Tool.UVPan => UVOpKind.Pan,
		Tool.UVScale => UVOpKind.Scale,
		Tool.UVRotate => UVOpKind.Rotate,
		_ => UVOpKind.None,
	};

	private void BeginUVOp( Ray ray, UVOpKind kind )
	{
		_uvOpFaces.Clear();
		var touched = new HashSet<ModelBrush>();
		foreach ( var (brush, triangle) in _selectedFaces )
		{
			if ( brush is null || !brush.IsValid() ) continue;
			if ( !brush.CanLocalEdit() ) continue;
			var mesh = brush.PolygonMesh;
			if ( mesh is null ) continue;
			var face = mesh.TriangleToFace( triangle );

			mesh.ComputeFaceNormal( face, out var localNormal );
			var worldNormal = (brush.WorldRotation * localNormal).Normal;
			if ( worldNormal.LengthSquared < _axisEpsilon ) continue;
			var verts = mesh.GetFaceVertices( face );
			if ( verts is null || verts.Length == 0 ) continue;

			var planePoint = brush.WorldTransform.PointToWorld( mesh.GetVertexPosition( verts[0] ) );
			var plane = new Plane( planePoint, worldNormal );
			var hit = plane.IntersectLine( ray.Position, ray.Position + ray.Forward * MaxRayDistance );
			if ( !hit.HasValue ) continue;

			// Face centroid in world space — pivot for scale (radial) and
			// rotate (angle around centroid in the face plane).
			var centroidLocal = Vector3.Zero;
			var vertCount = 0;
			foreach ( var v in verts )
			{
				centroidLocal += mesh.GetVertexPosition( v );
				vertCount++;
			}
			if ( vertCount > 0 ) centroidLocal /= vertCount;
			var centroidWorld = brush.WorldTransform.PointToWorld( centroidLocal );

			mesh.GetFaceTextureParameters( face, out var axisU, out var axisV, out _ );

			_uvOpFaces.Add( new UVOpFaceState
			{
				Brush = brush,
				Triangle = triangle,
				StartOffset = mesh.GetTextureOffset( face ),
				StartScale = mesh.GetTextureScale( face ),
				StartAxisU = axisU,
				StartAxisV = axisV,
				StartHit = hit.Value,
				AxisU = new Vector3( axisU.x, axisU.y, axisU.z ),
				AxisV = new Vector3( axisV.x, axisV.y, axisV.z ),
				PlanePoint = planePoint,
				PlaneNormal = worldNormal,
				FaceCentroidWorld = centroidWorld,
			} );
			touched.Add( brush );
		}
		if ( _uvOpFaces.Count == 0 ) return;
		_undo.BeginEdit( touched );
		_uvOpActive = kind;

		if ( kind == UVOpKind.Rotate )
		{
			SeedUVRotateReference();
		}
	}

	// Pick a reference face for measuring the cursor's swept angle. All
	// faces in _uvOpFaces already passed the ray-plane test at press, so
	// the first one with a non-degenerate press vector is fine. The angle
	// is measured in the reference face's plane using its world axisU /
	// axisV so the angular delta is consistent with how the texture
	// rotation appears on the surface the user clicked on.
	private void SeedUVRotateReference()
	{
		_uvRotateStartRotation = _uvRotation;
		_uvRotateAccumulatedDeg = 0f;
		_uvRotatePrevAngleDeg = 0f;
		_uvRotateRefValid = false;
		foreach ( var state in _uvOpFaces )
		{
			var pressVec = state.StartHit - state.FaceCentroidWorld;
			if ( pressVec.LengthSquared < _axisEpsilon ) continue;
			_uvRotateRefCentroid = state.FaceCentroidWorld;
			_uvRotateRefPlanePoint = state.PlanePoint;
			_uvRotateRefPlaneNormal = state.PlaneNormal;
			_uvRotateRefAxisU = state.AxisU;
			_uvRotateRefAxisV = state.AxisV;
			_uvRotatePrevAngleDeg = MathF.Atan2(
				Vector3.Dot( pressVec, state.AxisV ),
				Vector3.Dot( pressVec, state.AxisU ) ) * 180f / MathF.PI;
			_uvRotateRefValid = true;
			break;
		}
	}

	private void ApplyUVOp( Ray ray )
	{
		switch ( _uvOpActive )
		{
			case UVOpKind.Pan: ApplyUVPanOp( ray ); break;
			case UVOpKind.Scale: ApplyUVScaleOp( ray ); break;
			case UVOpKind.Rotate: ApplyUVRotateOp( ray ); break;
			default: return;
		}
		RebuildUVOpFaces();
	}

	// Move each face's offset by the world-space drag projected onto that
	// face's own U / V axes. Subtract du / dv because adding to offset
	// shifts the sample forward along U / V, which visually moves the
	// texture the opposite way; subtracting makes it follow the cursor.
	private void ApplyUVPanOp( Ray ray )
	{
		foreach ( var state in _uvOpFaces )
		{
			if ( !TryProjectRayOntoFace( ray, state, out var hit ) ) continue;
			var worldDelta = hit - state.StartHit;
			var du = Vector3.Dot( worldDelta, state.AxisU );
			var dv = Vector3.Dot( worldDelta, state.AxisV );
			var mesh = state.Brush.PolygonMesh;
			var face = mesh.TriangleToFace( state.Triangle );
			mesh.SetTextureOffset( face, state.StartOffset - new Vector2( du, dv ) );
		}
	}

	// Single radial-scale factor derived from one reference face, applied
	// uniformly to every selected face. Previously each face computed its
	// own factor against its own centroid, so multi-face selections drifted
	// onto different scales and ReadUVAgreement collapsed to NaN, leaving
	// the UV TILE entries stuck on "—". Matching the NumberEntry's "one
	// value across the selection" semantics — newScale = refStartScale *
	// factor, written to every face — keeps the entries in agreement and
	// re-rendering as the drag progresses.
	private void ApplyUVScaleOp( Ray ray )
	{
		if ( _uvOpFaces.Count == 0 ) return;
		var reference = _uvOpFaces[0];
		var plane = new Plane( reference.PlanePoint, reference.PlaneNormal );
		var hit = plane.IntersectLine( ray.Position, ray.Position + ray.Forward * MaxRayDistance );
		if ( !hit.HasValue ) return;
		var startRadius = (reference.StartHit - reference.FaceCentroidWorld).Length;
		if ( startRadius < _axisEpsilon ) return;
		var newRadius = (hit.Value - reference.FaceCentroidWorld).Length;
		var factor = MathF.Max( _minScaleFactor, newRadius / startRadius );
		var newScale = reference.StartScale * factor;

		foreach ( var state in _uvOpFaces )
		{
			if ( state.Brush is null || !state.Brush.IsValid() ) continue;
			var mesh = state.Brush.PolygonMesh;
			if ( mesh is null ) continue;
			var face = mesh.TriangleToFace( state.Triangle );
			mesh.SetTextureScale( face, newScale );
		}
	}

	// Rotate axisU / axisV around the face normal by the angle swept from
	// (press → cursor) about the face centroid, measured in the face plane.
	// Single rotation delta across all selected faces, matching the UV
	// ROTATION NumberEntry's "one uniform angle" semantics. The delta is
	// the cursor's accumulated angle around the seeded reference face's
	// centroid (so the user can spin past 360°), applied as a rotation
	// of every face's snapshot axes around its own plane normal. Mirrors
	// the existing UVRotation property: keeps _uvRotation in sync so the
	// toolbar entry re-renders with the new value.
	private void ApplyUVRotateOp( Ray ray )
	{
		if ( !_uvRotateRefValid ) return;
		var plane = new Plane( _uvRotateRefPlanePoint, _uvRotateRefPlaneNormal );
		var hit = plane.IntersectLine( ray.Position, ray.Position + ray.Forward * MaxRayDistance );
		if ( !hit.HasValue ) return;
		var curVec = hit.Value - _uvRotateRefCentroid;
		if ( curVec.LengthSquared < _axisEpsilon ) return;

		var curAngleDeg = MathF.Atan2(
			Vector3.Dot( curVec, _uvRotateRefAxisV ),
			Vector3.Dot( curVec, _uvRotateRefAxisU ) ) * 180f / MathF.PI;
		// Per-frame delta wrapped to [-180, 180] so a 359° → 0° crossing
		// looks like a 1° step instead of -359°, then accumulated so
		// multi-revolution drags read past 360.
		var frameDelta = curAngleDeg - _uvRotatePrevAngleDeg;
		while ( frameDelta > 180f ) frameDelta -= 360f;
		while ( frameDelta < -180f ) frameDelta += 360f;
		_uvRotateAccumulatedDeg += frameDelta;
		_uvRotatePrevAngleDeg = curAngleDeg;

		foreach ( var state in _uvOpFaces )
		{
			if ( state.Brush is null || !state.Brush.IsValid() ) continue;
			var mesh = state.Brush.PolygonMesh;
			if ( mesh is null ) continue;
			// Mirror RotateSelectedFacesTangent's exact axis choice — the
			// cross of the stored U / V axes, not the face's geometric
			// normal — so dragging the tool 17.3° produces the same axisU
			// / axisV pair as typing 17.3 into the NumberEntry on the same
			// starting state.
			var startUVec = (Vector3)state.StartAxisU;
			var startVVec = (Vector3)state.StartAxisV;
			var rotAxis = Vector3.Cross( startUVec, startVVec ).Normal;
			var rotation = Rotation.FromAxis( rotAxis, _uvRotateAccumulatedDeg );
			var newU = (startUVec * rotation).Normal;
			var newV = (startVVec * rotation).Normal;
			var face = mesh.TriangleToFace( state.Triangle );
			var scale = mesh.GetTextureScale( face );
			mesh.SetFaceTextureParameters( face,
				new Vector4( newU, state.StartAxisU.w ),
				new Vector4( newV, state.StartAxisV.w ),
				scale );
		}

		// Drive the same scalar the UV ROTATION NumberEntry binds to so
		// the entry refreshes through the toolbar's BuildHash (it already
		// hashes UVRotation). Setting the backing field directly bypasses
		// the property's setter — the setter would call RotateSelected
		// FacesTangent against the CURRENT axes, double-rotating on top
		// of the snapshot-based rotation we just applied above.
		var normalized = (_uvRotateStartRotation + _uvRotateAccumulatedDeg) % 360f;
		if ( normalized < 0f ) normalized += 360f;
		_uvRotation = normalized;
	}

	private static bool TryProjectRayOntoFace( Ray ray, in UVOpFaceState state, out Vector3 hit )
	{
		hit = default;
		if ( state.Brush is null || !state.Brush.IsValid() ) return false;
		var plane = new Plane( state.PlanePoint, state.PlaneNormal );
		var p = plane.IntersectLine( ray.Position, ray.Position + ray.Forward * MaxRayDistance );
		if ( !p.HasValue ) return false;
		hit = p.Value;
		return true;
	}

	// Re-derive per-vertex texcoords from the updated UV params and rebuild
	// each touched brush's mesh component so the change is visible this
	// frame. Shared by all three UV ops.
	private void RebuildUVOpFaces()
	{
		var groups = new Dictionary<ModelBrush, HashSet<FaceHandle>>();
		foreach ( var state in _uvOpFaces )
		{
			if ( state.Brush is null || !state.Brush.IsValid() ) continue;
			var mesh = state.Brush.PolygonMesh;
			if ( mesh is null ) continue;
			if ( !groups.TryGetValue( state.Brush, out var faces ) )
			{
				faces = new HashSet<FaceHandle>();
				groups[state.Brush] = faces;
			}
			faces.Add( mesh.TriangleToFace( state.Triangle ) );
		}
		foreach ( var (brush, faces) in groups )
		{
			brush.PolygonMesh.ComputeFaceTextureCoordinatesFromParameters( faces );
			RebuildBrushMeshComponent( brush );
		}
	}

	private void EndUVOp( bool commit )
	{
		if ( _uvOpActive == UVOpKind.None )
		{
			_uvOpFaces.Clear();
			return;
		}
		if ( commit )
		{
			// One RPC per face on release; per-tick would flood the network.
			foreach ( var state in _uvOpFaces )
			{
				if ( state.Brush is null || !state.Brush.IsValid() ) continue;
				if ( !state.Brush.CanLocalEdit() ) continue;
				var mesh = state.Brush.PolygonMesh;
				if ( mesh is null ) continue;
				var face = mesh.TriangleToFace( state.Triangle );
				// Scale and offset cover Pan and Scale. For Rotate the axes
				// themselves changed; broadcast those via RpcSetFaceTextureAxes
				// (scale tags along so peers don't have to read the existing
				// value out of band).
				if ( _uvOpActive == UVOpKind.Rotate )
				{
					mesh.GetFaceTextureParameters( face, out var axisU, out var axisV, out _ );
					var scale = mesh.GetTextureScale( face );
					state.Brush.RpcSetFaceTextureAxes( state.Triangle, axisU, axisV, scale );
				}
				else
				{
					var scale = mesh.GetTextureScale( face );
					var offset = mesh.GetTextureOffset( face );
					state.Brush.RpcSetFaceUVScaleOffset( state.Triangle, scale, offset );
				}
			}
			_undo.EndEdit();
		}
		else
		{
			_undo.CancelEdit();
		}
		_uvOpFaces.Clear();
		_uvOpActive = UVOpKind.None;
	}

	private void RotateSelectedFacesTangent( float deltaDeg )
	{
		if ( _selectedFaces.Count == 0 || deltaDeg == 0f ) return;

		var touched = _selectedFaces.Select( f => f.Item1 ).Where( b => b is not null ).Distinct().ToList();
		if ( touched.Count > 0 ) _undo.BeginEdit( touched );
		foreach ( var (brush, triangle) in _selectedFaces )
		{
			if ( !brush.CanLocalEdit() ) continue;
			var mesh = brush.PolygonMesh;
			var face = mesh.TriangleToFace( triangle );

			mesh.GetFaceTextureParameters( face, out var axisU, out var axisV, out var scale );

			var newAxisU = (Vector3)axisU;
			var newAxisV = (Vector3)axisV;
			var axis = Vector3.Cross( newAxisU, newAxisV ).Normal;
			var rotation = Rotation.FromAxis( axis, deltaDeg );
			newAxisU = (newAxisU * rotation).Normal;
			newAxisV = (newAxisV * rotation).Normal;

			var newU = new Vector4( newAxisU, axisU.w );
			var newV = new Vector4( newAxisV, axisV.w );

			mesh.SetFaceTextureParameters( face, newU, newV, scale );
			brush.RpcSetFaceTextureAxes( triangle, newU, newV, scale );
		}

		foreach ( var (brush, _) in GroupSelectedFacesByBrush() )
		{
			if ( !brush.CanLocalEdit() ) continue;
			RebuildBrushMeshComponent( brush );
		}
		if ( touched.Count > 0 ) _undo.EndEdit();
	}

	public bool CanFitTexture => !IsPlayMode && Selection == SelectionMode.Face && _selectedFaces.Count > 0;


	// "Make every selected face show the texture exactly once, edge to edge."
	//
	// Delegates to the engine's PolygonMesh.JustifyFaceTextureParameters with
	// TextureJustification.Fit — the same primitive the s&box editor's own
	// "Fit" button is built on. Trying to derive the right scale / offset
	// ourselves never reproduced the editor result because the formula
	// involves the engine-reported texture size (e.g. 256 for the stand-in
	// template albedo) which the user-facing toolbar tileScale doesn't see.
	// Letting the engine pick the params is the simplest way to match the
	// editor's behavior exactly.
	//
	// Follow with Left + Top justification so the fitted region is anchored
	// at the (0, 0) UV corner instead of floating in the middle of UV space,
	// then run ComputeFaceTextureCoordinatesFromParameters per brush to
	// regenerate the per-vertex coords from the new params. Mirror the
	// editor's structure (DoFit in s&box's SceneEditorSession) for that
	// last sequence too.
	public void FitTextureAction()
	{
		if ( !CanFitTexture ) return;

		var touched = _selectedFaces.Select( f => f.Item1 ).Where( b => b is not null ).Distinct().ToList();
		if ( touched.Count == 0 ) return;
		_undo.BeginEdit( touched );

		// Group selected faces by brush so each call into the engine APIs
		// processes one mesh at a time.
		var perBrushFaces = new Dictionary<ModelBrush, HashSet<FaceHandle>>();
		foreach ( var (brush, triangle) in _selectedFaces )
		{
			if ( !brush.CanLocalEdit() ) continue;
			var mesh = brush.PolygonMesh;
			if ( mesh is null ) continue;
			var face = mesh.TriangleToFace( triangle );
			if ( !perBrushFaces.TryGetValue( brush, out var set ) )
			{
				set = new HashSet<FaceHandle>();
				perBrushFaces[brush] = set;
			}
			set.Add( face );
		}

		foreach ( var (brush, faces) in perBrushFaces )
		{
			var mesh = brush.PolygonMesh;
			// Each face is treated independently (extents = null). The
			// editor's "treat selection as one face" toggle is the only
			// thing that would change here — pass a unioned FaceExtents
			// instead of null and the same fit covers the whole batch as
			// one quad's worth of texture.
			mesh.JustifyFaceTextureParameters( faces, PolygonMesh.TextureJustification.Fit, null );
			mesh.JustifyFaceTextureParameters( faces, PolygonMesh.TextureJustification.Left, null );
			mesh.JustifyFaceTextureParameters( faces, PolygonMesh.TextureJustification.Top, null );
			mesh.ComputeFaceTextureCoordinatesFromParameters( faces );
			RebuildBrushMeshComponent( brush );
		}

		// Broadcast the new state per face. Two RPCs cover the split:
		// axes + faceScale via RpcSetFaceTextureAxes (Justify rewrites
		// these), and the resulting tileScale / offset via RpcSetFaceUV
		// ScaleOffset (Left / Top push the offset for corner alignment).
		foreach ( var (brush, triangle) in _selectedFaces )
		{
			if ( !brush.CanLocalEdit() ) continue;
			var mesh = brush.PolygonMesh;
			if ( mesh is null ) continue;
			var face = mesh.TriangleToFace( triangle );
			if ( !perBrushFaces.TryGetValue( brush, out var set ) || !set.Contains( face ) ) continue;

			mesh.GetFaceTextureParameters( face, out var axisU, out var axisV, out var faceScale );
			var tileScale = mesh.GetTextureScale( face );
			var offset = mesh.GetTextureOffset( face );

			brush.RpcSetFaceTextureAxes( triangle, axisU, axisV, faceScale );
			brush.RpcSetFaceUVScaleOffset( triangle, tileScale, offset );
		}

		_undo.EndEdit();
	}

	public void RealignTextureAction()
	{
		if ( IsPlayMode ) return;
		if ( _selectedFaces.Count == 0 ) return;

		var touched = _selectedFaces.Select( f => f.Item1 ).Where( b => b is not null ).Distinct().ToList();
		if ( touched.Count > 0 ) _undo.BeginEdit( touched );
		foreach ( var (brush, triangle) in _selectedFaces )
		{
			if ( !brush.CanLocalEdit() ) continue;
			var mesh = brush.PolygonMesh;
			var face = mesh.TriangleToFace( triangle );

			mesh.ComputeFaceNormal( face, out var localNormal );
			var worldNormal = (brush.WorldRotation * localNormal).Normal;
			var (axisU, axisV) = ModelBrush.DefaultUVAxesForNormal( worldNormal );

			var scale = mesh.GetTextureScale( face );
			var newU = new Vector4( axisU, 0f );
			var newV = new Vector4( axisV, 0f );

			mesh.SetFaceTextureParameters( face, newU, newV, scale );
			brush.RpcSetFaceTextureAxes( triangle, newU, newV, scale );
		}

		ApplyUV();
		if ( touched.Count > 0 ) _undo.EndEdit();
	}

	#endregion

	#region Undo / Redo

	public void UndoAction() => _undo.Undo();
	public void RedoAction() => _undo.Redo();

	internal void OnUndoRedoApplied()
	{
		_selectedBrushes.RemoveWhere( b => b is null || !b.IsValid() );
		_selectedFaces.RemoveWhere( t => t.Item1 is null || !t.Item1.IsValid() );
		_selectedEdges.RemoveWhere( t => t.Item1 is null || !t.Item1.IsValid() );
		// Vertex selection is keyed by polygon-mesh vertex index, but undo
		// restores a freshly-deserialized mesh whose indices are not
		// guaranteed to match the pre-undo mesh. Drop stale entries.
		_selectedVertices.Clear();
		_selectedCloud.RemoveWhere( c => c is null || !c.IsValid() );

		_gizmoActiveAxis = GizmoAxis.None;
		_gizmoDragStartPositions.Clear();
		_gizmoDragStartPositionsCloud.Clear();
		_vertexDragStartLocal.Clear();
		_vertexDragAnchor = null;
		_gizmoShiftPendingClone = false;
		_rotActiveAxis = GizmoAxis.None;
		_rotDragStartTransforms.Clear();
		_rotDragStartTransformsCloud.Clear();
		_rotDragBasisOverride = null;
		_scaleActiveSide = GizmoAxisSide.None;
		_scaleDragStartTransforms.Clear();
		_scaleDragStartTransformsCloud.Clear();
	}

	#endregion
}
