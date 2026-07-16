using Sandbox;
using System;

// Marks a GameObject as a cloud-spawned asset (a model prop, or a sprite
// placeholder that stands in for a scripted entity in editor mode). Player
// selection and undo treat these like brushes, but with a reduced action set
// for entity placeholders.
public sealed class CloudInstance : Component
{
	public enum AssetKind
	{
		Model,
		Entity,
		// MAP-imported entity that doesn't come from the s&box cloud (e.g. a
		// Quake `info_player_start`). Rendered as a 16×16×16 wireframe box so
		// the user can select and move it; on export it round-trips back to a
		// MAP entity with the same classname and properties.
		MapEntity,
	}

	// Process-wide registry of live cloud instances, used by the editor to draw
	// per-instance overlays (e.g. the forward-direction arrow on entity
	// placeholders) without paying for a scene-wide component search each frame.
	public static readonly HashSet<CloudInstance> All = new();

	[Property]
	public AssetKind Kind { get; set; }

	// The package FullIdent the instance was spawned from; kept for future
	// play-mode hand-off (entity placeholder -> real NPC). For Kind=MapEntity
	// this is empty; for Kind=Entity / Model it is also written back to the
	// MAP file as the `sbox_workshop` property on export so a re-import can
	// resolve the same cloud asset.
	[Property]
	public string PackageIdent { get; set; }

	// Sprite thumbnail url for entity placeholders. Sync'd so late joiners can
	// recover the visual.
	[Property]
	public string ThumbUrl { get; set; }

	// MAP-style entity classname (`info_player_start`, `light`, ...). For
	// cloud-spawned entities we emit a generic `sbox_workshop_entity` on
	// export when this is empty so the MAP retains *something* readable; the
	// real recovery key is the `sbox_workshop` property carried alongside.
	[Property]
	public string ClassName { get; set; } = "";

	// MAP entity properties as parallel string lists so the [Property]
	// machinery serialises and network-syncs them via primitive types — we
	// don't depend on Dictionary<,> being a supported network property type.
	// Use the Properties helpers below to access them as KVPs.
	[Property]
	public List<string> PropertyKeys { get; set; } = new();
	[Property]
	public List<string> PropertyValues { get; set; } = new();

	public ModelRenderer Renderer { get; set; }

	public bool IsEntityPlaceholder => Kind == AssetKind.Entity || Kind == AssetKind.MapEntity;
	public bool IsMapEntity => Kind == AssetKind.MapEntity;

	#region Properties (MAP key/value pairs)

	public bool TryGetProperty( string key, out string value )
	{
		value = null;
		if ( string.IsNullOrEmpty( key ) || PropertyKeys is null || PropertyValues is null ) return false;
		var count = Math.Min( PropertyKeys.Count, PropertyValues.Count );
		for ( var i = 0; i < count; i++ )
		{
			if ( !string.Equals( PropertyKeys[i], key, StringComparison.OrdinalIgnoreCase ) ) continue;
			value = PropertyValues[i];
			return true;
		}
		return false;
	}

	public string GetProperty( string key, string defaultValue = "" ) =>
		TryGetProperty( key, out var v ) ? v : defaultValue;

	public void SetProperty( string key, string value )
	{
		if ( string.IsNullOrEmpty( key ) ) return;
		PropertyKeys ??= new List<string>();
		PropertyValues ??= new List<string>();
		var count = Math.Min( PropertyKeys.Count, PropertyValues.Count );
		for ( var i = 0; i < count; i++ )
		{
			if ( !string.Equals( PropertyKeys[i], key, StringComparison.OrdinalIgnoreCase ) ) continue;
			PropertyValues[i] = value ?? "";
			return;
		}
		PropertyKeys.Add( key );
		PropertyValues.Add( value ?? "" );
	}

	public bool RemoveProperty( string key )
	{
		if ( string.IsNullOrEmpty( key ) || PropertyKeys is null || PropertyValues is null ) return false;
		var count = Math.Min( PropertyKeys.Count, PropertyValues.Count );
		for ( var i = 0; i < count; i++ )
		{
			if ( !string.Equals( PropertyKeys[i], key, StringComparison.OrdinalIgnoreCase ) ) continue;
			PropertyKeys.RemoveAt( i );
			PropertyValues.RemoveAt( i );
			return true;
		}
		return false;
	}

	public void SetProperties( IEnumerable<KeyValuePair<string, string>> properties )
	{
		PropertyKeys = new List<string>();
		PropertyValues = new List<string>();
		if ( properties is null ) return;
		foreach ( var kv in properties )
		{
			if ( string.IsNullOrEmpty( kv.Key ) ) continue;
			PropertyKeys.Add( kv.Key );
			PropertyValues.Add( kv.Value ?? "" );
		}
	}

	public IEnumerable<KeyValuePair<string, string>> EnumerateProperties()
	{
		if ( PropertyKeys is null || PropertyValues is null ) yield break;
		var count = Math.Min( PropertyKeys.Count, PropertyValues.Count );
		for ( var i = 0; i < count; i++ )
		{
			yield return new KeyValuePair<string, string>( PropertyKeys[i], PropertyValues[i] );
		}
	}

	#endregion

	protected override void OnStart()
	{
		base.OnStart();
		Tags.Add( "world" );
		All.Add( this );
	}

	protected override void OnDestroy()
	{
		base.OnDestroy();
		All.Remove( this );
	}

	// Network-wide soft destroy used by the editor's undo system. We can't
	// recreate a cloud-loaded asset synchronously during undo, so deletes are
	// represented as Enabled = false; toggling here mirrors that on every peer.
	[Rpc.Broadcast]
	public void RpcSetActive( bool active )
	{
		if ( GameObject is null ) return;
		GameObject.Enabled = active;
	}
}
