import * as std from "std";
import * as os from 'os';

import { Webview } from "./webview.so";

const html = `
<div>
  <button id="increment">+</button>
  <button id="decrement">−</button>
  <span>Counter: <span id="counterResult">0</span></span>
</div>
<hr />
<div>
  <button id="compute">Compute</button>
  <span>Result: <span id="computeResult">(not started)</span></span>
</div>
<hr />
<div>
	<label for="random">Random number: </label><output label="random"></output>
</div>
<script type="module">
  const getElements = ids => Object.assign({}, ...ids.map(
    id => ({ [id]: document.getElementById(id) })));
  const ui = getElements([
    "increment", "decrement", "counterResult", "compute",
    "computeResult"
  ]);
  ui.increment.addEventListener("click", async () => {
    ui.counterResult.textContent = await window.count(1);
  });
  ui.decrement.addEventListener("click", async () => {
    ui.counterResult.textContent = await window.count(-1);
  });
  ui.compute.addEventListener("click", async () => {
    ui.compute.disabled = true;
    ui.computeResult.textContent = "(pending)";
    ui.computeResult.textContent = await window.compute(6, 7);
    ui.compute.disabled = false;
  });
	window.updateRandom = ( n ) => {
		document.querySelector( 'output' ).innerHTML = n;
	}
	window.addEventListener("DOMContentLoaded", () => {
		window.ready();
	});
</script>`;

const w = new Webview( 1 );
w.setTitle( "Bind Example" );
w.setSize( 600, 400 );

// state that lived in context_t.count is just a closure variable here
let count = 0;

w.bind( "count", ( direction ) => {
	count += direction;
	return count;
} );

w.bind( "compute", ( a, b ) => {
	return a * b;
} );

w.bind( 'webview', () => { return true; } );

w.bind( "ready", () => { update(); } );

function update(){
	w.eval( `updateRandom( ${ parseInt( Math.random() * 1000 ) } )` );
}

w.setHtml( html );
w.run();
w.destroy();
