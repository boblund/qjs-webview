export { SpaNavCe };

/// #if WEBPACK
// #code import sheet from './spanav.css?stylesheet';
/// #else
import sheet from './spanav.css' with { type: 'css' };
/// #endif

class SpaNavCe extends HTMLElement {
	#tagNodes = [];

	constructor() {
		super();
		// Attach a shadow root for style encapsulation
		this.attachShadow( { mode: 'open' } );
		this.shadowRoot.adoptedStyleSheets = [sheet];
		this.shadowRoot.innerHTML = `<nav></nav>`;
	}

	connectedCallback() {
		setTimeout( () => this.#configure(), 0 );
	}

	get children(){ return this.#tagNodes.reduce( ( a, v ) => {
		a[ v.id ] = v;
		return a;
		}, {} );
	};

	#configure(){
		this.#tagNodes = [];
		const nav = this.shadowRoot.querySelector( 'nav' );
		let underlinedEl;
		const firstLink = this.querySelector( 'a, button' );
    let currentEl = document.querySelector( `div[data-spa="${ firstLink.id }"]` );
    currentEl.classList.add( 'active' );

		for( const n of this.querySelectorAll( 'a, button' ) ){
			this.#tagNodes.push( n );
			if( n.tagName != 'A' ) continue;
			n.addEventListener( 'click', ( e ) => {
				e.preventDefault();
				currentEl.classList.remove( 'active' );
				document.querySelector('spa-nav').shadowRoot.getElementById(currentEl.dataset.spa).style.textDecoration = '';
				currentEl = document.querySelector( `div[data-spa="${ e.target.id }"]` );
				currentEl.classList.add( 'active' );
				//if( underlinedEl ) underlinedEl.style.textDecoration = '';
				if( e.target.id != firstLink.id ){
					e.target.style.textDecoration = 'underline';
					//underlinedEl = e.target;
				//} else {
					//underlinedEl = undefined;
				}
			} );
		}

		nav.appendChild( this.#tagNodes.shift() );
		if( this.#tagNodes.length < 4 ){
			for( const n of this.#tagNodes ){ nav.appendChild( n ); };
		} else {
			nav.insertAdjacentHTML( 'beforeend', `
				<div class="dropdown">
					<a href="#" id="ddClick" style="padding: 0.25em; margin-right: 0.75em; font-size: 30px;">&#8801</a>
					<div class="dropdown-menu" ></div>
				</div>
			` );
			const dropDownMenu = nav.querySelector( 'div.dropdown-menu' );
			for( const n of this.#tagNodes ){
				dropDownMenu.appendChild( n );
			};
		}
		const dropdown = nav.querySelector( '.dropdown' );
		if( dropdown ){
			const button = nav.querySelector( '#ddClick' );
			const menu = nav.querySelector( '.dropdown-menu' );

			// Toggle dropdown menu
			button.addEventListener( 'click', ( e ) => {
				e.stopPropagation();
				menu.classList.toggle( 'show' );
			} );

			// Close dropdown when clicking outside
			window.addEventListener( 'click', ( e ) => {
				if ( !dropdown.contains( e.target ) ) {
					menu.classList.remove( 'show' );
				}
			} );
		}
	}
}
