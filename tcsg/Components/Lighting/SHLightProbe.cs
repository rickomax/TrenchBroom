using System;
using System.Threading.Tasks;

namespace Sandbox;

[Title( "SH Light Probe" )]
public sealed class SHLightProbe : Component, Component.ExecuteInEditor
{
	[Property, Range( 16, 128, 16 )]
	public int CaptureResolution { get; set; } = 64;

	[Property, ReadOnly]
	public Vector4 CoefficientsR { get; set; }

	[Property, ReadOnly]
	public Vector4 CoefficientsG { get; set; }

	[Property, ReadOnly]
	public Vector4 CoefficientsB { get; set; }

	const float SH_C0 = 0.282095f;
	const float SH_C1 = 0.488603f;

	static readonly Rotation[] FaceRotations =
	{
		Rotation.LookAt( Vector3.Forward ),
		Rotation.LookAt( Vector3.Backward ),
		Rotation.LookAt( Vector3.Left ),
		Rotation.LookAt( Vector3.Right ),
		Rotation.LookAt( Vector3.Up ),
		Rotation.LookAt( Vector3.Down ),
	};

	[Button( "Bake", "lightbulb" )]
	public async Task Bake()
	{
		if ( Scene is null )
			return;

		var size = CaptureResolution;
		var probePosition = WorldPosition;

		var cameraGo = Scene.CreateObject();
		cameraGo.Name = "SHProbe_BakeCamera";
		var camera = cameraGo.AddComponent<CameraComponent>();
		camera.FieldOfView = 90f;
		camera.ZNear = 1f;
		camera.ZFar = 20000f;
		camera.BackgroundColor = Color.Black;
		camera.ClearFlags = ClearFlags.All;

		var renderTarget = Texture.CreateRenderTarget()
			.WithSize( size, size )
			.WithFormat( ImageFormat.RGBA16161616F )
			.Create();

		try
		{
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
				float normalization = 4f * MathF.PI / totalWeight;
				CoefficientsR = accumR * normalization;
				CoefficientsG = accumG * normalization;
				CoefficientsB = accumB * normalization;
			}
		}
		finally
		{
			renderTarget?.Dispose();
			cameraGo.Destroy();
		}
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

		for ( int py = 0; py < size; py++ )
		{
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

				accumR += basis * (r * weight);
				accumG += basis * (g * weight);
				accumB += basis * (b * weight);
				totalWeight += weight;
			}
		}
	}

	public Vector3 EvaluateIrradiance( Vector3 normal )
	{
		const float A0 = 3.141593f;
		const float A1 = 2.094395f;

		float ir = A0 * SH_C0 * CoefficientsR.x
			+ A1 * SH_C1 * (CoefficientsR.y * normal.x + CoefficientsR.z * normal.y + CoefficientsR.w * normal.z);

		float ig = A0 * SH_C0 * CoefficientsG.x
			+ A1 * SH_C1 * (CoefficientsG.y * normal.x + CoefficientsG.z * normal.y + CoefficientsG.w * normal.z);

		float ib = A0 * SH_C0 * CoefficientsB.x
			+ A1 * SH_C1 * (CoefficientsB.y * normal.x + CoefficientsB.z * normal.y + CoefficientsB.w * normal.z);

		return new Vector3( MathF.Max( ir, 0f ), MathF.Max( ig, 0f ), MathF.Max( ib, 0f ) );
	}

	protected override void DrawGizmos()
	{
		const int hSegments = 24;
		const int vSegments = 12;
		const float radius = 30f;

		for ( int y = 0; y < vSegments; y++ )
		{
			for ( int x = 0; x < hSegments; x++ )
			{
				var v0 = SphereVertex( x, y, hSegments, vSegments, radius );
				var v1 = SphereVertex( x + 1, y, hSegments, vSegments, radius );
				var v2 = SphereVertex( x, y + 1, hSegments, vSegments, radius );
				var v3 = SphereVertex( x + 1, y + 1, hSegments, vSegments, radius );

				var normal = ((v0 + v1 + v2 + v3) / 4f).Normal;
				Gizmo.Draw.Color = EvaluateIrradiance( normal );

				Gizmo.Draw.SolidTriangle( v0, v1, v2 );
				Gizmo.Draw.SolidTriangle( v1, v3, v2 );
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
}
