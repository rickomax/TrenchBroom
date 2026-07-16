using System;
using Sandbox.UI;

public class ResizeHandle : Panel
{
	bool _dragging;
	float _lastMouseX;

	public Action OnDragStarted { get; set; }
	public Action<float> OnDragMoved { get; set; } // delta x in UI (post-scale) units
	public Action OnDragEnded { get; set; }

	public override void OnButtonEvent( ButtonEvent value )
	{
		base.OnButtonEvent( value );

		if ( value.Button != "mouseleft" ) return;

		if ( value.Pressed )
		{
			_dragging = true;
			_lastMouseX = Mouse.Position.x;
			OnDragStarted?.Invoke();
		}
		else if ( _dragging )
		{
			_dragging = false;
			OnDragEnded?.Invoke();
		}
	}

	public override void Tick()
	{
		base.Tick();
		if ( !_dragging ) return;

		var x = Mouse.Position.x;
		var dxScreen = x - _lastMouseX;
		if ( dxScreen == 0 ) return;
		_lastMouseX = x;

		// Mouse.Position is in screen pixels; Style.Width sits in UI units
		// (screen-pixels / ScreenPanel.Scale). Convert so a 1px mouse move
		// produces a 1px panel resize regardless of the active UI scale.
		var dx = dxScreen * ScaleFromScreen;
		OnDragMoved?.Invoke( dx );
	}

	// Lets the consumer stop tracking mid-drag (e.g. when the resized panel
	// hits a min/max limit). Without this the mouse keeps drifting past the
	// clamp point and the next reverse motion has to "catch back up" before
	// the panel edge moves again.
	public void EndDrag()
	{
		if ( !_dragging ) return;
		_dragging = false;
		OnDragEnded?.Invoke();
	}
}
