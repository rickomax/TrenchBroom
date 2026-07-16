using System;
using System.Runtime.InteropServices;
using System.Threading.Tasks;

namespace Sandbox;

[Title( "SH Light Volume" )]
public sealed class SHLightVolume : Component, Component.ExecuteInEditor
{
	[Property]
	public BBox Bounds
	{
		get;
		set
		{
			if ( field == value ) return;
			field = value;
			MarkDirty();
		}
	} = BBox.FromPositionAndSize( Vector3.Zero, new Vector3( 512.0f ) );

	[Property, Range( 8, 256 )]
	public float ProbeSpacing
	{
		get;
		set
		{
			if ( field == value ) return;
			field = value;
			MarkDirty();
		}
	} = 64.0f;

	[Property, Range( 16, 128, 16 )]
	public int CaptureResolution { get; set; } = 32;

	[Property, Range( 1, 8 )]
	public int Bounces { get; set; } = 1;

	public enum DebugViewMode
	{
		None,
		HashCells,
		IndirectOnly,
		VisibilityMask,
		RawTrilinear
	}

	[Property]
	public DebugViewMode DebugView
	{
		get;
		set
		{
			if ( field == value ) return;
			field = value;
			MarkDirty();
		}
	} = DebugViewMode.None;

	[Property, Range( 0, 50 )]
	public float NormalBias
	{
		get;
		set
		{
			if ( field == value ) return;
			field = value;
			MarkDirty();
		}
	} = 5.0f;

	[Property, Range( -1, 0 )]
	public float BackfaceMin
	{
		get;
		set
		{
			if ( field == value ) return;
			field = value;
			MarkDirty();
		}
	} = -0.5f;

	[Property, Range( 0, 1 )]
	public float BackfaceMax
	{
		get;
		set
		{
			if ( field == value ) return;
			field = value;
			MarkDirty();
		}
	} = 0.2f;

	[Property, Range( 0, 50 )]
	public float LuminanceContrast
	{
		get;
		set
		{
			if ( field == value ) return;
			field = value;
			MarkDirty();
		}
	} = 5.0f;

	[Property, Range( 4, 128 )]
	public float VisibilityCellSize
	{
		get;
		set
		{
			if ( field == value ) return;
			field = value;
		}
	} = 16.0f;

	[Property, Range( 128, 1024 )]
	public int VisibilityHashSize
	{
		get;
		set
		{
			if ( field == value ) return;
			field = value;
		}
	} = 256;

	[Property, Range( 1, 5 )]
	public int VisibilityRaysPerProbe
	{
		get;
		set
		{
			if ( field == value ) return;
			field = value;
		}
	} = 3;

	[Property, Hide]
	public Texture SHTextureR { get; set; }

	[Property, Hide]
	public Texture SHTextureG { get; set; }

	[Property, Hide]
	public Texture SHTextureB { get; set; }

	[Property, Hide]
	public Texture ValidityTexture { get; set; }

	[Property, Hide]
	public Texture VisibilityHashTexture { get; set; }

	public Vector3Int ProbeCounts => ComputeProbeCounts();

	const float SH_C0 = 0.282095f;
	const float SH_C1 = 0.488603f;

	const int ValidityRayCount = 64;
	const float BackfaceThreshold = 0.25f;

	static readonly Rotation[] FaceRotations =
	{
		Rotation.LookAt( Vector3.Forward ),
		Rotation.LookAt( Vector3.Backward ),
		Rotation.LookAt( Vector3.Left ),
		Rotation.LookAt( Vector3.Right ),
		Rotation.LookAt( Vector3.Up ),
		Rotation.LookAt( Vector3.Down ),
	};

	private float[] probeValidity;
	private Vector3Int bakedCounts;
	private float bakedCellSize;

	[Button( "Fit to Scene Bounds", "fullscreen" )]
	public void ExtendToSceneBounds()
	{
		if ( Scene is null )
			return;

		WorldScale = 1;
		WorldRotation = Rotation.Identity;
		var sceneBounds = BBox.FromPositionAndSize( WorldPosition );

		foreach ( var renderer in Scene.GetAll<Renderer>() )
		{
			if ( renderer is not IHasBounds bounds )
				continue;
			sceneBounds = sceneBounds.AddBBox( bounds.LocalBounds.Transform( renderer.WorldTransform ) );
		}
		foreach ( var terrain in Scene.GetAll<Terrain>() )
		{
			var collision = terrain.EnableCollision;
			terrain.EnableCollision = true;
			sceneBounds = sceneBounds.AddBBox( terrain.GetWorldBounds() );
			if ( !collision )
				terrain.EnableCollision = false;
		}
		foreach ( var mesh in Scene.GetAll<MeshComponent>() )
		{
			var model = mesh.Model;
			if ( model is null )
				continue;
			sceneBounds = sceneBounds.AddBBox( model.RenderBounds.Transform( mesh.WorldTransform ) );
		}
		sceneBounds = sceneBounds.Translate( -WorldPosition ).Grow( 16 );

		Bounds = sceneBounds;
	}

	[Button( "Clear", "delete" )]
	public void Clear()
	{
		SHTextureR = null;
		SHTextureG = null;
		SHTextureB = null;
		ValidityTexture = null;
		VisibilityHashTexture = null;
		probeValidity = null;
		MarkDirty();
	}

	[Button( "Bake", "lightbulb" )]
	public async Task Bake()
	{
		if ( Scene is null )
			return;

		// Clear previous bake data so it doesn't feed back into the new captures
		SHTextureR = null;
		SHTextureG = null;
		SHTextureB = null;
		ValidityTexture = null;
		VisibilityHashTexture = null;
		probeValidity = null;
		MarkDirty();

		var counts = ProbeCounts;
		var size = CaptureResolution;
		var totalProbes = counts.x * counts.y * counts.z;

		var texR = Texture.CreateVolume( counts.x, counts.y, counts.z, ImageFormat.RGBA16161616F )
			.WithName( "SHVolume_R" )
			.WithUAVBinding()
			.WithMips( 1 )
			.Finish();
		var texG = Texture.CreateVolume( counts.x, counts.y, counts.z, ImageFormat.RGBA16161616F )
			.WithName( "SHVolume_G" )
			.WithUAVBinding()
			.WithMips( 1 )
			.Finish();
		var texB = Texture.CreateVolume( counts.x, counts.y, counts.z, ImageFormat.RGBA16161616F )
			.WithName( "SHVolume_B" )
			.WithUAVBinding()
			.WithMips( 1 )
			.Finish();
		var texValidity = Texture.CreateVolume( counts.x, counts.y, counts.z, ImageFormat.RGBA16161616F )
			.WithName( "SHVolume_Validity" )
			.WithUAVBinding()
			.WithMips( 1 )
			.Finish();

		var coeffsR = new Vector4[totalProbes];
		var coeffsG = new Vector4[totalProbes];
		var coeffsB = new Vector4[totalProbes];

		var validityValues = new float[totalProbes];
		ComputeProbeValidity( counts, validityValues );

		var cameraGo = Scene.CreateObject();
		cameraGo.Name = "SHVolume_BakeCamera";
		var camera = cameraGo.AddComponent<CameraComponent>( false );
		camera.FieldOfView = 90f;
		camera.ZNear = 1f;
		camera.ZFar = 20000f;
		camera.BackgroundColor = Color.Black;
		camera.ClearFlags = ClearFlags.All;

#if NO_COMPUTE
		var shDataR = new Half[totalProbes * 4];
		var shDataG = new Half[totalProbes * 4];
		var shDataB = new Half[totalProbes * 4];

		var renderTarget = Texture.CreateRenderTarget()
			.WithSize( size, size )
			.WithFormat( ImageFormat.RGBA16161616F )
			.Create();
#else
		var faceTargets = new Texture[6];
		for ( int i = 0; i < 6; i++ )
			faceTargets[i] = Texture.CreateRenderTarget()
				.WithSize( size, size )
				.WithFormat( ImageFormat.RGBA16161616F )
				.Create();

		var cs = new ComputeShader( "common/SHVolume/sh_project_cs" );
#endif

		try
		{
#if NO_COMPUTE
			int probeIndex = 0;

			for ( int z = 0; z < counts.z; z++ )
			{
				for ( int y = 0; y < counts.y; y++ )
				{
					for ( int x = 0; x < counts.x; x++ )
					{
						var probePosition = GetProbeWorldPosition( new Vector3Int( x, y, z ) );

						if ( validityValues[probeIndex] <= 0f )
						{
							probeIndex++;
							continue;
						}

						Vector4 accumR = Vector4.Zero;
						Vector4 accumG = Vector4.Zero;
						Vector4 accumB = Vector4.Zero;
						float totalWeight = 0f;

						for ( int face = 0; face < 6; face++ )
						{
							renderTarget.Clear( Color.Black );

							camera.RenderToTexture( renderTarget, new Rendering.ViewSetup
							{
								Transform = new Transform( probePosition, FaceRotations[face] ),
								FieldOfView = 90f,
								EnablePostprocessing = false,
							} );

							await Task.Yield();

							var pixels = new Half[size * size * 4];
							renderTarget.GetPixels<Half>( (0, 0, size, size), 0, 0, pixels, ImageFormat.RGBA16161616F );

							AccumulateFaceSH( pixels, size, FaceRotations[face], ref accumR, ref accumG, ref accumB, ref totalWeight );
						}

						if ( totalWeight > 0f )
						{
							float norm = 4f * MathF.PI / totalWeight;
							var finalR = accumR * norm;
							var finalG = accumG * norm;
							var finalB = accumB * norm;

							int offset = probeIndex * 4;
							shDataR[offset + 0] = (Half)finalR.x;
							shDataR[offset + 1] = (Half)finalR.y;
							shDataR[offset + 2] = (Half)finalR.z;
							shDataR[offset + 3] = (Half)finalR.w;

							shDataG[offset + 0] = (Half)finalG.x;
							shDataG[offset + 1] = (Half)finalG.y;
							shDataG[offset + 2] = (Half)finalG.z;
							shDataG[offset + 3] = (Half)finalG.w;

							shDataB[offset + 0] = (Half)finalB.x;
							shDataB[offset + 1] = (Half)finalB.y;
							shDataB[offset + 2] = (Half)finalB.z;
							shDataB[offset + 3] = (Half)finalB.w;

							coeffsR[probeIndex] = finalR;
							coeffsG[probeIndex] = finalG;
							coeffsB[probeIndex] = finalB;
						}

						probeIndex++;
					}
				}
			}

			texR.Update( shDataR );
			texG.Update( shDataG );
			texB.Update( shDataB );

			SHTextureR = texR;
			SHTextureG = texG;
			SHTextureB = texB;
			MarkDirty();
#else
			for ( int bounce = 0; bounce < Bounces; bounce++ )
			{
				int probeIndex = 0;

				for ( int z = 0; z < counts.z; z++ )
				{
					for ( int y = 0; y < counts.y; y++ )
					{
						for ( int x = 0; x < counts.x; x++ )
						{
							var probePosition = GetProbeWorldPosition( new Vector3Int( x, y, z ) );

							if ( validityValues[probeIndex] <= 0f )
							{
								probeIndex++;
								continue;
							}

							for ( int face = 0; face < 6; face++ )
							{
								faceTargets[face].Clear( Color.Black );

								camera.RenderToTexture( faceTargets[face], new Rendering.ViewSetup
								{
									Transform = new Transform( probePosition, FaceRotations[face] ),
									FieldOfView = 90f,
									EnablePostprocessing = false,
								} );

								await Task.Yield();
							}

							var attributes = new RenderAttributes();
							for ( int face = 0; face < 6; face++ )
							cs.Attributes.Set( $"Face{face}", faceTargets[face] );
							cs.Attributes.Set( "SHVolumeR", texR );
							cs.Attributes.Set( "SHVolumeG", texG );
							cs.Attributes.Set( "SHVolumeB", texB );
							cs.Attributes.Set( "ProbeIndex", new Vector3Int( x, y, z ) );
							cs.Attributes.Set( "FaceSize", size );
							cs.Dispatch( 256, 1, 1 );

							probeIndex++;
						}
					}
				}

				SHTextureR = texR;
				SHTextureG = texG;
				SHTextureB = texB;
				MarkDirty();
			}

			// Read back SH coefficients for visibility hash luminance calculation
			var shReadR = new Half[totalProbes * 4];
			var shReadG = new Half[totalProbes * 4];
			var shReadB = new Half[totalProbes * 4];
			texR.GetPixels3D<Half>( (0, 0, 0, counts.x, counts.y, counts.z), 0, shReadR, ImageFormat.RGBA16161616F );
			texG.GetPixels3D<Half>( (0, 0, 0, counts.x, counts.y, counts.z), 0, shReadG, ImageFormat.RGBA16161616F );
			texB.GetPixels3D<Half>( (0, 0, 0, counts.x, counts.y, counts.z), 0, shReadB, ImageFormat.RGBA16161616F );
			for ( int i = 0; i < totalProbes; i++ )
			{
				coeffsR[i] = new Vector4( (float)shReadR[i * 4], (float)shReadR[i * 4 + 1], (float)shReadR[i * 4 + 2], (float)shReadR[i * 4 + 3] );
				coeffsG[i] = new Vector4( (float)shReadG[i * 4], (float)shReadG[i * 4 + 1], (float)shReadG[i * 4 + 2], (float)shReadG[i * 4 + 3] );
				coeffsB[i] = new Vector4( (float)shReadB[i * 4], (float)shReadB[i * 4 + 1], (float)shReadB[i * 4 + 2], (float)shReadB[i * 4 + 3] );
			}
#endif

			var validityHalf = new Half[totalProbes * 4];
			for ( int i = 0; i < totalProbes; i++ )
			{
				validityHalf[i * 4 + 0] = (Half)validityValues[i];
				validityHalf[i * 4 + 1] = (Half)0f;
				validityHalf[i * 4 + 2] = (Half)0f;
				validityHalf[i * 4 + 3] = (Half)0f;
			}
			texValidity.Update( validityHalf );
			ValidityTexture = texValidity;

			var hashSize = VisibilityHashSize;
			var hashData = new byte[hashSize * hashSize * 4];
			for ( int i = 0; i < hashSize * hashSize; i++ )
				hashData[i * 4 + 3] = 0xFF;
			ComputeVisibilityHash( counts, validityValues, coeffsR, coeffsG, coeffsB, hashSize, hashData );

			var texVisHash = Texture.Create( hashSize, hashSize )
				.WithName( "SHVolume_VisHash" )
				.WithFormat( ImageFormat.RGBA8888 )
				.WithData( hashData )
				.Finish();
			VisibilityHashTexture = texVisHash;

			probeValidity = validityValues;
			bakedCounts = counts;
		}
		finally
		{
#if NO_COMPUTE
			renderTarget?.Dispose();
#else
			for ( int i = 0; i < 6; i++ )
				faceTargets[i]?.Dispose();
#endif
			cameraGo.Destroy();
		}

		MarkDirty();
	}

	static int SpatialHash( int x, int y, int z, int tableSize )
	{
		unchecked
		{
			int h = x * 73856093 ^ y * 19349663 ^ z * 83492791;
			return ((h % (tableSize * tableSize)) + (tableSize * tableSize)) % (tableSize * tableSize);
		}
	}

	private void ComputeVisibilityHash( Vector3Int probeCounts, float[] validity, Vector4[] coeffsR, Vector4[] coeffsG, Vector4[] coeffsB, int hashSize, byte[] hashData )
	{
		var raysPerProbe = VisibilityRaysPerProbe;
		var spacing = ComputeSpacing( probeCounts );

		// Snap cell size to an exact divisor of the probe spacing so
		// hash cells never straddle a probe cell boundary.
		var divisions = MathF.Max( 1f, MathF.Ceiling( ProbeSpacing / VisibilityCellSize ) );
		bakedCellSize = ProbeSpacing / divisions;
		var cellSize = bakedCellSize;

		var maxProbeDistance = spacing.Length * 1.5f;

		// Hash cells are in local space anchored to BBoxMin so they
		// align with the probe grid (both share the same origin).
		var localSize = Bounds.Size;
		var minCell = new Vector3Int( 0, 0, 0 );
		var maxCell = new Vector3Int(
			(int)MathF.Ceiling( localSize.x / cellSize ),
			(int)MathF.Ceiling( localSize.y / cellSize ),
			(int)MathF.Ceiling( localSize.z / cellSize )
		);

		// Precompute probe world positions
		var probePositions = new Vector3[probeCounts.x * probeCounts.y * probeCounts.z];
		for ( int pz = 0; pz < probeCounts.z; pz++ )
			for ( int py = 0; py < probeCounts.y; py++ )
				for ( int px = 0; px < probeCounts.x; px++ )
					probePositions[px + py * probeCounts.x + pz * probeCounts.x * probeCounts.y] =
						GetProbeWorldPosition( new Vector3Int( px, py, pz ) );

		// Precompute probe luminance (DC term)
		var probeLuminance = new float[probeCounts.x * probeCounts.y * probeCounts.z];
		for ( int i = 0; i < probeLuminance.Length; i++ )
			probeLuminance[i] = 0.2126f * coeffsR[i].x + 0.7152f * coeffsG[i].x + 0.0722f * coeffsB[i].x;

		// Ray spread offsets for multi-ray per probe
		var spreadOffsets = new Vector3[raysPerProbe];
		spreadOffsets[0] = Vector3.Zero;
		if ( raysPerProbe > 1 )
		{
			var spreadDirs = GenerateSphericalDirections( raysPerProbe - 1 );
			for ( int i = 0; i < spreadDirs.Length; i++ )
				spreadOffsets[i + 1] = spreadDirs[i] * (cellSize * 0.3f);
		}

		var recipSpacing = new Vector3(
			spacing.x > 0 ? 1f / spacing.x : 0,
			spacing.y > 0 ? 1f / spacing.y : 0,
			spacing.z > 0 ? 1f / spacing.z : 0
		);

		Sandbox.Utility.Parallel.For( minCell.z, maxCell.z + 1, cz =>
		{
			for ( int cy = minCell.y; cy <= maxCell.y; cy++ )
			{
				for ( int cx = minCell.x; cx <= maxCell.x; cx++ )
				{
					// Cell center in local space, anchored to BBoxMin
					var localCellCenter = Bounds.Mins + new Vector3(
						(cx + 0.5f) * cellSize,
						(cy + 0.5f) * cellSize,
						(cz + 0.5f) * cellSize
					);
					var cellCenter = WorldTransform.PointToWorld( localCellCenter );

					// Find which trilinear cell this point falls in
					var gridPos = (localCellCenter - Bounds.Mins) * recipSpacing;
					var baseCoord = new Vector3Int(
						Math.Clamp( (int)MathF.Floor( gridPos.x ), 0, probeCounts.x - 2 ),
						Math.Clamp( (int)MathF.Floor( gridPos.y ), 0, probeCounts.y - 2 ),
						Math.Clamp( (int)MathF.Floor( gridPos.z ), 0, probeCounts.z - 2 )
					);

					byte mask = 0;
					bool insideGeometry = false;
					for ( int i = 0; i < 8; i++ )
					{
						var offset = new Vector3Int( i & 1, (i >> 1) & 1, (i >> 2) & 1 );
						var probeCoord = baseCoord + offset;
						var probeIdx = probeCoord.x + probeCoord.y * probeCounts.x + probeCoord.z * probeCounts.x * probeCounts.y;

						if ( validity[probeIdx] <= 0f )
							continue;

						var probeWorld = probePositions[probeIdx];

						// Multi-ray: require majority to pass
						int passCount = 0;
						bool hitBackface = false;
						for ( int r = 0; r < raysPerProbe; r++ )
						{
							var origin = cellCenter + spreadOffsets[r];
							var trace = Scene.Trace
								.Ray( origin, probeWorld )
								.UsePhysicsWorld( true )
								.Run();


							if ( !trace.Hit )
								passCount++;
							else if ( trace.StartedSolid )
								hitBackface = true;
						}

						if ( hitBackface )
						{
							insideGeometry = true;
							break;
						}

						if ( passCount * 2 >= raysPerProbe )
						{
							int bit = (probeCoord.x & 1) | ((probeCoord.y & 1) << 1) | ((probeCoord.z & 1) << 2);
							mask |= (byte)(1 << bit);
						}
					}

					if ( insideGeometry )
					{
						mask = 0;
					}

					if ( mask == 0xFF)
						continue;

					var hash = SpatialHash( cx, cy, cz, hashSize );
					var totalSlots = hashSize * hashSize;
					var cellX = (byte)(cx & 0xFF);
					var cellY = (byte)(cy & 0xFF);
					var cellZ = (byte)(cz & 0xFF);

					// Linear probing: try up to 4 consecutive slots
					for ( int bucket = 0; bucket < 4; bucket++ )
					{
						int slot = ((hash + bucket) % totalSlots) * 4;
						// Empty slot (alpha == 0xFF means default/empty)
						if ( hashData[slot + 3] == 0xFF )
						{
							hashData[slot + 0] = cellX;
							hashData[slot + 1] = cellY;
							hashData[slot + 2] = cellZ;
							hashData[slot + 3] = mask;
							break;
						}
						// Already our cell (update)
						if ( hashData[slot + 0] == cellX && hashData[slot + 1] == cellY && hashData[slot + 2] == cellZ )
						{
							hashData[slot + 3] = mask;
							break;
						}
					}
				}
			}
		} );
	}

	private void ComputeProbeValidity( Vector3Int counts, float[] validityValues )
	{
		var spacing = ComputeSpacing( counts );
		var minSpacing = MathF.Min( spacing.x, MathF.Min( spacing.y, spacing.z ) );
		var traceDistance = minSpacing;
		var directions = GenerateSphericalDirections( ValidityRayCount );

		Sandbox.Utility.Parallel.For( 0, counts.z, z =>
		{
			for ( int y = 0; y < counts.y; y++ )
			{
				for ( int x = 0; x < counts.x; x++ )
				{
					var index = x + y * counts.x + z * counts.x * counts.y;
					var probePosition = GetProbeWorldPosition( new Vector3Int( x, y, z ) );

					int backfaceCount = 0;
					int hitCount = 0;

					for ( int r = 0; r < directions.Length; r++ )
					{
						var endPoint = probePosition + directions[r] * traceDistance;
						var trace = Scene.Trace
							.Ray( probePosition, endPoint )
							.UsePhysicsWorld( true )
							.Run();

						if ( !trace.Hit )
							continue;

						hitCount++;
						if ( Vector3.Dot( trace.Normal, directions[r] ) > 0f )
							backfaceCount++;
					}

					float backfaceRatio = hitCount > 0 ? (float)backfaceCount / hitCount : 0f;
					validityValues[index] = backfaceRatio >= BackfaceThreshold ? 0f : 1f;
				}
			}
		} );
	}

	private static Vector3[] GenerateSphericalDirections( int count )
	{
		var directions = new Vector3[count];
		var goldenRatio = (1.0f + MathF.Sqrt( 5.0f )) / 2.0f;
		var angleIncrement = MathF.PI * 2.0f * goldenRatio;

		for ( int i = 0; i < count; i++ )
		{
			var t = (float)i / count;
			var inclination = MathF.Acos( 1.0f - 2.0f * t );
			var azimuth = angleIncrement * i;

			var sinInc = MathF.Sin( inclination );
			directions[i] = new Vector3(
				sinInc * MathF.Cos( azimuth ),
				sinInc * MathF.Sin( azimuth ),
				MathF.Cos( inclination )
			);
		}

		return directions;
	}

	private static void AccumulateFaceSH(
		Half[] pixels, int size,
		Rotation faceRotation,
		ref Vector4 accumR, ref Vector4 accumG, ref Vector4 accumB,
		ref float totalWeight )
	{
		var forward = faceRotation.Forward;
		var right = faceRotation.Right;
		var up = faceRotation.Up;

		var rowAccumR = new Vector4[size];
		var rowAccumG = new Vector4[size];
		var rowAccumB = new Vector4[size];
		var rowWeights = new float[size];

		Sandbox.Utility.Parallel.For( 0, size, py =>
		{
			Vector4 localR = Vector4.Zero;
			Vector4 localG = Vector4.Zero;
			Vector4 localB = Vector4.Zero;
			float localWeight = 0f;

			for ( int px = 0; px < size; px++ )
			{
				float u = (px + 0.5f) / size * 2f - 1f;
				float v = (py + 0.5f) / size * 2f - 1f;

				var dir = (forward + u * right - v * up).Normal;

				float distSq = 1f + u * u + v * v;
				float weight = 1f / (distSq * MathF.Sqrt( distSq ));

				int idx = (py * size + px) * 4;
				float r = (float)pixels[idx + 0];
				float g = (float)pixels[idx + 1];
				float b = (float)pixels[idx + 2];

				var basis = new Vector4( SH_C0, SH_C1 * dir.x, SH_C1 * dir.y, SH_C1 * dir.z );

				localR += basis * (r * weight);
				localG += basis * (g * weight);
				localB += basis * (b * weight);
				localWeight += weight;
			}

			rowAccumR[py] = localR;
			rowAccumG[py] = localG;
			rowAccumB[py] = localB;
			rowWeights[py] = localWeight;
		} );

		for ( int py = 0; py < size; py++ )
		{
			accumR += rowAccumR[py];
			accumG += rowAccumG[py];
			accumB += rowAccumB[py];
			totalWeight += rowWeights[py];
		}
	}

	internal bool BuildGpuData( out SHVolumeSystem.SHVolumeGpuData data )
	{
		data = default;

		if ( !SHTextureR.IsValid() || !SHTextureG.IsValid() || !SHTextureB.IsValid() || !ValidityTexture.IsValid() || !VisibilityHashTexture.IsValid() )
			return false;

		var probeCounts = ProbeCounts;
		var spacing = ComputeSpacing( probeCounts );

		var worldMatrix = Matrix.CreateScale( WorldScale )
			* Matrix.CreateRotation( WorldRotation )
			* Matrix.CreateTranslation( WorldPosition );

		data = new SHVolumeSystem.SHVolumeGpuData
		{
			Transform = worldMatrix.Inverted,
			BBoxMin = Bounds.Mins,
			BBoxMax = Bounds.Maxs,
			NormalBias = NormalBias,
			ProbeSpacing = spacing,
			ReciprocalSpacing = new(
				spacing.x > 0.0f ? 1.0f / spacing.x : 0.0f,
				spacing.y > 0.0f ? 1.0f / spacing.y : 0.0f,
				spacing.z > 0.0f ? 1.0f / spacing.z : 0.0f
			),
			ProbeCounts = probeCounts,
			SHTextureRIndex = SHTextureR.Index,
			SHTextureGIndex = SHTextureG.Index,
			SHTextureBIndex = SHTextureB.Index,
			ValidityTextureIndex = ValidityTexture.Index,
			VisHashTextureIndex = VisibilityHashTexture.Index,
			VisHashCellSize = bakedCellSize > 0 ? bakedCellSize : VisibilityCellSize,
			VisHashTableSize = VisibilityHashSize,
			BackfaceMin = BackfaceMin,
			BackfaceMax = BackfaceMax,
			LuminanceContrast = LuminanceContrast,
			DebugMode = (int)DebugView,
		};

		return true;
	}

	public Vector3 GetProbeWorldPosition( Vector3Int index )
	{
		return WorldTransform.PointToWorld( GetProbeLocalPosition( index ) );
	}

	private Vector3 GetProbeLocalPosition( Vector3Int index )
	{
		var spacing = ComputeSpacing( ProbeCounts );
		return Bounds.Mins + index * spacing;
	}

	private Vector3 ComputeSpacing( Vector3Int counts )
	{
		return new Vector3( ProbeSpacing, ProbeSpacing, ProbeSpacing );
	}

	private Vector3Int ComputeProbeCounts()
	{
		const int minProbes = 2;
		const int maxProbes = 40;

		var size = Bounds.Size;
		var d = MathF.Max( ProbeSpacing, 1f );

		return new Vector3Int(
			Math.Clamp( (int)MathF.Round( size.x / d ) + 1, minProbes, maxProbes ),
			Math.Clamp( (int)MathF.Round( size.y / d ) + 1, minProbes, maxProbes ),
			Math.Clamp( (int)MathF.Round( size.z / d ) + 1, minProbes, maxProbes )
		);
	}

	protected override void OnEnabled()
	{
		base.OnEnabled();
		Transform.OnTransformChanged += MarkDirty;
		MarkDirty();
	}

	protected override void OnDisabled()
	{
		base.OnDisabled();
		Transform.OnTransformChanged -= MarkDirty;
		MarkDirty();
	}

	private void MarkDirty()
	{
		Scene?.Get<SHVolumeSystem>()?.MarkDirty();
	}

	protected override void DrawGizmos()
	{
		if ( !Gizmo.IsSelected )
			return;

		var bounds = Bounds;
		Gizmo.Control.BoundingBox( "Bounds", bounds, out bounds );
		Gizmo.Draw.LineBBox( bounds );
		Gizmo.Draw.Color = new Color( 1.0f, 0.9f, 0.25f, 0.05f );
		Gizmo.Draw.SolidBox( bounds );
		Bounds = bounds;

		if ( probeValidity == null )
			return;

		var counts = bakedCounts;
		var spacing = ComputeSpacing( counts );
		var minSpacing = MathF.Min( spacing.x, MathF.Min( spacing.y, spacing.z ) );
		var radius = minSpacing * 0.3f;

		int probeIndex = 0;
		for ( int z = 0; z < counts.z; z++ )
		{
			for ( int y = 0; y < counts.y; y++ )
			{
				for ( int x = 0; x < counts.x; x++ )
				{
					var localPos = Bounds.Mins + new Vector3Int( x, y, z ) * spacing;

					if ( probeValidity[probeIndex] <= 0f )
					{
						Gizmo.Draw.Color = Color.Red.WithAlpha( 0.4f );
						Gizmo.Draw.SolidSphere( localPos, radius * 0.15f, 4, 4 );
					}
					else
					{
						Gizmo.Draw.Color = Color.Green.WithAlpha( 0.5f );
						Gizmo.Draw.SolidSphere( localPos, radius * 0.3f, 4, 4 );
					}

					probeIndex++;
				}
			}
		}
	}

	private static Vector3 SphereVertex( int x, int y, int hSeg, int vSeg, float radius )
	{
		float u = (float)x / hSeg;
		float v = (float)y / vSeg;
		float hAngle = u * MathF.PI * 2f;
		float vAngle = v * MathF.PI;

		return new Vector3(
			radius * MathF.Sin( vAngle ) * MathF.Cos( hAngle ),
			radius * MathF.Sin( vAngle ) * MathF.Sin( hAngle ),
			radius * MathF.Cos( vAngle )
		);
	}

	static readonly Color[] RoomColors = new Color[]
	{
		new(0.90f,0.20f,0.30f), new(0.20f,0.70f,0.35f), new(0.25f,0.45f,0.95f), new(0.95f,0.75f,0.10f),
		new(0.80f,0.25f,0.85f), new(0.10f,0.85f,0.80f), new(0.95f,0.50f,0.10f), new(0.55f,0.30f,0.90f),
		new(0.40f,0.85f,0.20f), new(0.90f,0.35f,0.60f), new(0.15f,0.60f,0.75f), new(0.85f,0.85f,0.20f),
		new(0.60f,0.15f,0.50f), new(0.30f,0.90f,0.55f), new(0.95f,0.40f,0.40f), new(0.20f,0.35f,0.80f),
		new(0.75f,0.65f,0.10f), new(0.45f,0.20f,0.75f), new(0.15f,0.80f,0.45f), new(0.85f,0.30f,0.15f),
		new(0.35f,0.55f,0.90f), new(0.70f,0.80f,0.25f), new(0.90f,0.20f,0.70f), new(0.20f,0.90f,0.85f),
		new(0.80f,0.55f,0.30f), new(0.30f,0.25f,0.65f), new(0.55f,0.90f,0.35f), new(0.95f,0.30f,0.50f),
		new(0.10f,0.50f,0.60f), new(0.65f,0.75f,0.45f), new(0.75f,0.15f,0.35f), new(0.25f,0.80f,0.65f),
		new(0.50f,0.40f,0.85f), new(0.85f,0.60f,0.20f), new(0.35f,0.70f,0.80f), new(0.90f,0.45f,0.70f),
		new(0.15f,0.55f,0.40f), new(0.70f,0.30f,0.60f), new(0.40f,0.90f,0.70f), new(0.80f,0.20f,0.45f),
		new(0.25f,0.65f,0.55f), new(0.60f,0.50f,0.15f), new(0.45f,0.35f,0.90f), new(0.90f,0.70f,0.35f),
		new(0.30f,0.40f,0.70f), new(0.75f,0.85f,0.50f), new(0.55f,0.15f,0.70f), new(0.20f,0.85f,0.30f),
		new(0.85f,0.40f,0.25f), new(0.35f,0.75f,0.90f), new(0.65f,0.20f,0.45f), new(0.10f,0.70f,0.60f),
		new(0.80f,0.65f,0.50f), new(0.50f,0.30f,0.55f), new(0.30f,0.90f,0.40f), new(0.95f,0.25f,0.80f),
		new(0.40f,0.60f,0.25f), new(0.70f,0.45f,0.80f), new(0.15f,0.45f,0.85f), new(0.85f,0.80f,0.40f),
		new(0.60f,0.25f,0.30f), new(0.25f,0.85f,0.75f), new(0.90f,0.55f,0.55f), new(0.45f,0.50f,0.70f),
		new(0.35f,0.30f,0.50f), new(0.80f,0.35f,0.75f), new(0.50f,0.80f,0.45f), new(0.70f,0.60f,0.30f),
		new(0.20f,0.40f,0.90f), new(0.90f,0.15f,0.55f), new(0.40f,0.75f,0.60f), new(0.65f,0.40f,0.20f),
		new(0.15f,0.65f,0.85f), new(0.85f,0.50f,0.65f), new(0.55f,0.85f,0.55f), new(0.30f,0.20f,0.40f),
		new(0.75f,0.70f,0.60f), new(0.45f,0.55f,0.35f), new(0.90f,0.30f,0.90f), new(0.10f,0.75f,0.50f),
		new(0.60f,0.35f,0.75f), new(0.35f,0.90f,0.25f), new(0.80f,0.45f,0.45f), new(0.25f,0.30f,0.75f),
		new(0.70f,0.90f,0.35f), new(0.50f,0.20f,0.60f), new(0.95f,0.60f,0.45f), new(0.15f,0.85f,0.60f),
		new(0.75f,0.25f,0.20f), new(0.40f,0.65f,0.75f), new(0.85f,0.75f,0.30f), new(0.30f,0.50f,0.50f),
		new(0.65f,0.30f,0.85f), new(0.20f,0.60f,0.35f), new(0.90f,0.40f,0.15f), new(0.45f,0.80f,0.85f),
		new(0.55f,0.45f,0.25f), new(0.10f,0.35f,0.65f), new(0.80f,0.60f,0.75f), new(0.35f,0.45f,0.45f),
		new(0.70f,0.15f,0.50f), new(0.25f,0.75f,0.40f), new(0.95f,0.35f,0.35f), new(0.50f,0.70f,0.65f),
		new(0.40f,0.25f,0.80f), new(0.75f,0.55f,0.50f), new(0.15f,0.90f,0.70f), new(0.85f,0.20f,0.60f),
		new(0.60f,0.80f,0.20f), new(0.30f,0.60f,0.85f), new(0.90f,0.65f,0.55f), new(0.20f,0.45f,0.50f),
		new(0.65f,0.55f,0.40f), new(0.45f,0.15f,0.55f), new(0.80f,0.90f,0.45f), new(0.10f,0.30f,0.45f),
		new(0.55f,0.65f,0.80f), new(0.35f,0.85f,0.50f), new(0.90f,0.50f,0.85f), new(0.70f,0.35f,0.15f),
		new(0.25f,0.55f,0.70f), new(0.80f,0.30f,0.30f), new(0.40f,0.70f,0.40f), new(0.60f,0.45f,0.60f),
		new(0.15f,0.20f,0.55f), new(0.85f,0.70f,0.70f), new(0.50f,0.40f,0.30f), new(0.30f,0.80f,0.80f),
		new(0.75f,0.40f,0.55f), new(0.20f,0.50f,0.25f), new(0.95f,0.85f,0.60f), new(0.45f,0.30f,0.40f),
		new(0.65f,0.70f,0.75f), new(0.35f,0.15f,0.30f), new(0.80f,0.50f,0.15f), new(0.10f,0.65f,0.45f),
		new(0.55f,0.25f,0.80f), new(0.90f,0.80f,0.50f), new(0.40f,0.45f,0.60f), new(0.70f,0.55f,0.90f),
		new(0.25f,0.70f,0.20f), new(0.85f,0.15f,0.40f), new(0.50f,0.60f,0.50f), new(0.30f,0.35f,0.85f),
		new(0.75f,0.80f,0.35f), new(0.15f,0.40f,0.30f), new(0.60f,0.90f,0.60f), new(0.95f,0.45f,0.25f),
		new(0.20f,0.25f,0.70f), new(0.45f,0.75f,0.30f), new(0.80f,0.35f,0.85f), new(0.35f,0.60f,0.55f),
		new(0.65f,0.10f,0.65f), new(0.10f,0.80f,0.35f), new(0.90f,0.60f,0.80f), new(0.55f,0.50f,0.45f),
		new(0.30f,0.70f,0.65f), new(0.70f,0.25f,0.40f), new(0.40f,0.85f,0.75f), new(0.85f,0.45f,0.10f),
		new(0.15f,0.55f,0.80f), new(0.60f,0.65f,0.55f), new(0.25f,0.15f,0.45f), new(0.75f,0.50f,0.70f),
		new(0.50f,0.35f,0.20f), new(0.95f,0.20f,0.65f), new(0.20f,0.90f,0.55f), new(0.45f,0.40f,0.75f),
		new(0.80f,0.75f,0.55f), new(0.35f,0.50f,0.15f), new(0.65f,0.85f,0.45f), new(0.10f,0.25f,0.35f),
		new(0.90f,0.35f,0.20f), new(0.55f,0.70f,0.85f), new(0.30f,0.45f,0.25f), new(0.70f,0.60f,0.65f),
		new(0.40f,0.20f,0.50f), new(0.85f,0.55f,0.40f), new(0.15f,0.75f,0.70f), new(0.60f,0.30f,0.55f),
		new(0.25f,0.85f,0.45f), new(0.75f,0.40f,0.30f), new(0.50f,0.55f,0.90f), new(0.95f,0.70f,0.75f),
		new(0.20f,0.60f,0.50f), new(0.45f,0.25f,0.35f), new(0.80f,0.85f,0.65f), new(0.35f,0.35f,0.60f),
		new(0.65f,0.50f,0.30f), new(0.10f,0.45f,0.75f), new(0.90f,0.25f,0.45f), new(0.55f,0.80f,0.70f),
		new(0.30f,0.65f,0.40f), new(0.70f,0.45f,0.55f), new(0.40f,0.30f,0.65f), new(0.85f,0.65f,0.85f),
		new(0.15f,0.50f,0.20f), new(0.60f,0.75f,0.40f), new(0.25f,0.40f,0.55f), new(0.75f,0.30f,0.75f),
		new(0.50f,0.85f,0.30f), new(0.95f,0.55f,0.20f), new(0.20f,0.70f,0.85f), new(0.45f,0.60f,0.50f),
		new(0.80f,0.40f,0.60f), new(0.35f,0.25f,0.70f), new(0.65f,0.90f,0.55f), new(0.10f,0.35f,0.50f),
		new(0.90f,0.75f,0.40f), new(0.55f,0.40f,0.65f), new(0.30f,0.55f,0.30f), new(0.70f,0.20f,0.80f),
		new(0.40f,0.80f,0.55f), new(0.85f,0.30f,0.50f), new(0.15f,0.65f,0.35f), new(0.60f,0.55f,0.75f),
		new(0.25f,0.35f,0.20f), new(0.75f,0.60f,0.45f), new(0.50f,0.45f,0.55f), new(0.95f,0.40f,0.80f),
		new(0.20f,0.80f,0.40f), new(0.45f,0.70f,0.65f), new(0.80f,0.15f,0.25f), new(0.35f,0.40f,0.80f),
		new(0.65f,0.60f,0.20f), new(0.10f,0.90f,0.55f), new(0.90f,0.50f,0.30f), new(0.55f,0.30f,0.45f),
		new(0.30f,0.75f,0.75f), new(0.70f,0.40f,0.35f), new(0.40f,0.55f,0.80f), new(0.85f,0.70f,0.20f),
		new(0.15f,0.30f,0.60f), new(0.60f,0.85f,0.35f), new(0.25f,0.50f,0.45f), new(0.75f,0.35f,0.65f),
		new(0.50f,0.75f,0.50f), new(0.95f,0.65f,0.70f), new(0.20f,0.20f,0.30f), new(0.45f,0.90f,0.40f),
		new(0.80f,0.25f,0.55f), new(0.35f,0.65f,0.70f), new(0.65f,0.35f,0.50f),
	};

	private static Color RoomColor( int roomId )
	{
		if ( roomId < 0 ) return Color.Gray;
		return RoomColors[roomId % RoomColors.Length];
	}

	private static Vector3 EvaluateIrradiance( Vector4 coeffsR, Vector4 coeffsG, Vector4 coeffsB, Vector3 normal )
	{
		const float A0 = 3.141593f;
		const float A1 = 2.094395f;

		float ir = A0 * SH_C0 * coeffsR.x
			+ A1 * SH_C1 * (coeffsR.y * normal.x + coeffsR.z * normal.y + coeffsR.w * normal.z);
		float ig = A0 * SH_C0 * coeffsG.x
			+ A1 * SH_C1 * (coeffsG.y * normal.x + coeffsG.z * normal.y + coeffsG.w * normal.z);
		float ib = A0 * SH_C0 * coeffsB.x
			+ A1 * SH_C1 * (coeffsB.y * normal.x + coeffsB.z * normal.y + coeffsB.w * normal.z);

		return new Vector3( MathF.Max( ir, 0f ), MathF.Max( ig, 0f ), MathF.Max( ib, 0f ) );
	}
}
