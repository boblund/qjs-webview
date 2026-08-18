// index.mjs — webpack entry point, bundled to bundle.js
//debugger;
import './index.css';
import { BrumeCallCe, BrumeLoginCe, DialogCe, SpaNavCe, brumeStyleSheet } from './brume-ce/index.mjs';

customElements.define( 'brume-login', BrumeLoginCe );
customElements.define( 'brume-call', BrumeCallCe );
customElements.define( 'ce-dialog', DialogCe );
customElements.define( 'spa-nav', SpaNavCe );

await customElements.whenDefined( 'brume-login' );
await customElements.whenDefined( 'brume-call' );
await customElements.whenDefined( 'ce-dialog' );
await customElements.whenDefined( 'spa-nav' );

document.adoptedStyleSheets = [ ...document.adoptedStyleSheets, brumeStyleSheet ];

const brumeCall = document.querySelector( 'brume-call' );
const ceDialog = document.querySelector( 'ce-dialog' );
const divIdp = document.querySelector( 'div.idp' );
const brumeLogin = document.querySelector( 'brume-login' );
const loginPage = document.querySelector( 'div.login.page' );
const appContainer = document.querySelector( 'div.app-container' );
const divApp = document.querySelector( 'div.app' );
const nav = document.querySelector( 'spa-nav' );

let wsConnected = false;

async function dialog( type, msg ) { return await ceDialog[type]( msg ); }

function pcCleanup() {
	brumeCall.name = '';
	brumeCall.disconnected();
	appContainer.classList.add( 'hidden' );
	document.querySelector( '.output' ).innerHTML = '';
	if ( !wsConnected ) {
		document.querySelector( 'div[data-spa="p2p"]' ).classList.remove( 'active' );
		nav.shadowRoot.querySelector( '#p2p' ).style.display = 'none';
		document.querySelector( 'div[data-spa="login"]' ).classList.add( 'active' );
		nav.shadowRoot.querySelector( '#login' ).style.display = '';
	}
}

// -----------------------------------------------------------------------------
// native -> browser events
// -----------------------------------------------------------------------------

window.addEventListener( 'brume', ( e ) => {
	const { type, detail } = e.detail;
	switch ( type ) {
		case 'connect':
			brumeCall.connected();
			appContainer.classList.remove( 'hidden' );
			break;

		case 'data':
			document.querySelector( '.output' ).innerHTML =
                `message from ${ detail.peerName }: ${ detail.msg?.data ?? detail.msg }`;
			break;

		case 'disconnect':
			pcCleanup();
			break;

		case 'offer':
			( async () => {
				const accepted = await dialog( 'confirm', `Accept connection from ${ detail.from }` );
				if ( accepted ) brumeCall.name = detail.from;
				await window.brumeAnswer( accepted );
			} )();
			break;

		case 'peerError':
			if ( [ 'ENODEST', 'EBADDEST' ].includes( detail.code ) ) {
				dialog( 'alert', `Cannot connect to: ${ detail.peerUsername }` );
			}
			console.warn( `peerError: ${ JSON.stringify( detail ) }` );
			break;

		case 'wsClose':
			wsConnected = false;
			break;

		default:
			console.log( `unknown brume event: ${ type }` );
	}
} );

// -----------------------------------------------------------------------------
// UI wiring
// -----------------------------------------------------------------------------

brumeCall.callListener = async () => {
	if ( [ divIdp.innerHTML, '' ].includes( brumeCall.name ) ) {
		await dialog( 'alert', `Invalid username` );
		return;
	}
	await window.brumeCall( brumeCall.name );
};

brumeCall.hangupListener = async () => {
	await window.brumeHangup();
	pcCleanup();
};

window.addEventListener( 'load', () => {
	document.querySelector( 'body' ).classList.remove( 'hidden' );
} );

loginPage.classList.add( 'active' );
/*nav.querySelector( '#login' ).style.display = 'none';
loginPage.classList.remove( 'active' );
divApp.classList.add( 'active' );
loginPage.classList.add( 'p2p' ).style.display = '';*/

try {
	const token = await brumeLogin.getToken();
	const thisUser = await window.brumeStart( token );
	console.log( `index.mjs thisUser: ${ JSON.stringify( thisUser ) }` );
	wsConnected = true;
	divIdp.innerHTML = thisUser;
	loginPage.classList.remove( 'active' );
	divApp.classList.add( 'active' );
} catch ( e ) {
	console.error( `brumeStart failed: ${ e }` );
}
