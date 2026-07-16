using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;

namespace Sandbox;

sealed class SHVolumeSystem : GameObjectSystem<SHVolumeSystem>
{
	private GpuBuffer<SHVolumeGpuData> _gpuBuffer;
	private bool _dirty = true;

	public SHVolumeSystem( Scene scene ) : base( scene )
	{
		Listen( Stage.FinishUpdate, 0, UpdateVolumes, "UpdateSHVolumes" );
	}

	public override void Dispose()
	{
		ReleaseBuffer();

		Scene?.RenderAttributes?.Set( "SHVolume_Count", 0 );
		Scene?.RenderAttributes?.Set( "SHVolume_Data", (GpuBuffer)null );

		base.Dispose();
	}

	internal void MarkDirty()
	{
		_dirty = true;
	}

	private void UpdateVolumes()
	{
		if ( Application.IsHeadless || !_dirty )
			return;

		if ( Scene?.RenderAttributes is null )
			return;

		_dirty = false;

		var volumeData = new List<SHVolumeGpuData>();

		foreach ( var volume in Scene.GetAll<SHLightVolume>().Where( v => v is { Active: true, Enabled: true } ).OrderBy( v => v.Bounds.Volume ) )
		{
			if ( volume.BuildGpuData( out var data ) )
				volumeData.Add( data );
		}

		if ( volumeData.Count > 0 )
		{
			EnsureBufferCapacity( volumeData.Count );
			_gpuBuffer.SetData( volumeData );
			Scene.RenderAttributes.Set( "SHVolume_Count", volumeData.Count );
			Scene.RenderAttributes.Set( "SHVolume_Data", _gpuBuffer );
			return;
		}

		ReleaseBuffer();
		Scene.RenderAttributes.Set( "SHVolume_Count", 0 );
		Scene.RenderAttributes.Set( "SHVolume_Data", (GpuBuffer)null );
	}

	private void EnsureBufferCapacity( int count )
	{
		if ( _gpuBuffer is not null && _gpuBuffer.ElementCount >= count )
			return;

		ReleaseBuffer();
		_gpuBuffer = new GpuBuffer<SHVolumeGpuData>( Math.Max( count, 1 ), debugName: "SHVolume_Data" );
	}

	private void ReleaseBuffer()
	{
		_gpuBuffer?.Dispose();
		_gpuBuffer = null;
	}

	[StructLayout( LayoutKind.Sequential, Pack = 0 )]
	internal struct SHVolumeGpuData
	{
		public Matrix Transform;
		public Vector3 BBoxMin;
		public Vector3 BBoxMax;
		public float NormalBias;
		public Vector3 ProbeSpacing;
		public Vector3 ReciprocalSpacing;
		public Vector3Int ProbeCounts;
		public int SHTextureRIndex;
		public int SHTextureGIndex;
		public int SHTextureBIndex;
		public int ValidityTextureIndex;
		public int VisHashTextureIndex;
		public float VisHashCellSize;
		public int VisHashTableSize;
		public float BackfaceMin;
		public float BackfaceMax;
		public float LuminanceContrast;
		public int DebugMode;
	}
}
