/// #if !QUICKJS
import { AuthenticationDetails, CognitoUserPool, CognitoUser } from 'amazon-cognito-identity-js';
/// #endif

import brumeStyleSheet from './common.css?stylesheet' with { type: "css" };
import { SpaNavCe } from './spa-nav.mjs';

function base64url(input) {
  return btoa(input)
    .replace(/=/g, "")
    .replace(/\+/g, "-")
    .replace(/\//g, "_");
}

function buildJwt(payload) {
  const header = { alg: "none", typ: "JWT" };

  const encodedHeader = base64url(JSON.stringify(header));
  const encodedPayload = base64url(JSON.stringify(payload));

  // "none" alg means an empty signature segment
  return `${encodedHeader}.${encodedPayload}.`;
}

export{ BrumeCallCe, BrumeLoginCe, DialogCe, SpaNavCe, brumeStyleSheet };

class BrumeLoginCe extends HTMLElement {
	#email;
	#password;
	#checkbox;
	#stayLoggedInCb;
	#loginStatus;
	#loginCallback;

	constructor() {
		super();
		//this.hidden = true;
		this.attachShadow( { mode: 'open' } ).innerHTML = `
			<style>
				span[for="password"]:hover {
					cursor: pointer;
				}
			</style>

			<div style="margin-left: 1em;">
				<div id="emailDiv">
					<input type="text" placeholder="Email" id="email">
					<input type="checkbox" id="checkbox" name="checkbox">
					<label for="checkbox">Remember me</label>
				</div>

				<div id="passwordDiv">
					<input type="password" placeholder="Password" name="password" id="password"/>
					<span for="password">show</span>
				</div>

				<div id="stayLoggedInDiv">
					<input type="checkbox" id="stayLoggedInCb" name="stayLoggedInCb">
					<label for="stayLoggedInCb">Stay logged in</label>
				</div>

				</br><button id="submitLogin">submit</button>
				</br></br><div id="loginStatus"></div>
			</div>
		`;
		this.shadowRoot.adoptedStyleSheets = [ brumeStyleSheet ];
	}

	connectedCallback() {
		setTimeout( () => this.#configure(), 0 );
	}

	#configure() {
		const shadowRoot = this.shadowRoot;
		this.#email = this.shadowRoot.querySelector( '#email' );
		this.#password = this.shadowRoot.querySelector( '#password' );
		this.#checkbox = this.shadowRoot.querySelector( '#checkbox' );
		this.#stayLoggedInCb = this.shadowRoot.querySelector( '#stayLoggedInCb' );
		this.#loginStatus = this.shadowRoot.querySelector( '#loginStatus' );

		this.shadowRoot.querySelector( 'button' ).addEventListener( 'click', async () => {
			this.#loginStatus.innerHTML = '';
			if ( this.#checkbox.checked && this.#email.value !== "" ) {
				localStorage.email = this.#email.value;
				localStorage.checkbox = this.#checkbox.checked;
			} else {
				localStorage.email = "";
				localStorage.checkbox = "";
			}

			if( window?.LOCAL_BRUME || await window?.localBrume?.() ){
				const payload = { 'custom:brume_name': this.#email.value };
				const token = buildJwt( payload );
				this.#loginCallback( { token } );
				return;
			}

			const result = await this.#userPassAuth( this.#email.value, this.#password.value );
			if( result.error ) {
				if( result.error == "NEW_PASSWORD_REQUIRED" ) {
					alert( 'New Password Required. Change your password at brume.occams.solutions.' );
				} else { //if(result.error?.code == 'NotAuthorizedException'){
					this.#loginStatus.innerHTML = result.error;
				}
				delete localStorage.Authorization;
				throw( { error: result.error } );
			} else {
				if( this.#stayLoggedInCb.checked )
					localStorage.Authorization = result.IdToken;
				this.#loginCallback( { token: result.IdToken } );
			}
		} );

		this.shadowRoot.querySelector( 'span[for="password"]' ).addEventListener( 'click', ( event ) => {
			let password = shadowRoot.querySelector( `#${ event.target.attributes[ 'for' ].value }` ); //this.previousElementSibling;
			password.type = password.type === "password" ? "text" : "password";
			event.target.innerText = password.type === "password" ? "show" : "hide";
		} );
	}

	/// #if QUICKJS

	async #userPassAuth(username, password) {
		const r = await window.brumeLogin(username, password);
    return r;
	}

	/// #else

	#userPassAuth( username, password ) {
		const userPool = new CognitoUserPool( { UserPoolId: 'us-east-1_p5E3AsRc8', ClientId: '6dspdoqn9q00f0v42c12qvkh5l' } );
		return new Promise( ( res, rej ) => {
			const authenticationDetails = new AuthenticationDetails( { Username: username, Password: password } );
			const cognitoUser = new CognitoUser( { Username: username, Pool: userPool } );

			cognitoUser.authenticateUser( authenticationDetails, {
				onSuccess: function( result ) {
					cognitoUser.getUserAttributes( ( e, r ) => {
						res( { IdToken: result.getIdToken().getJwtToken() } );
					} );
				},
				onFailure: function( err ) {
					res ( { error: err } );
				}
			} );
		} );
	}

	/// #endif

	getToken() {
		this.hidden = false;
		return new Promise( ( res, rej ) => {
			this.#loginCallback = ( result ) => {
				if( result?.token ) {
					res( result.token );
					setTimeout( ()=> { this.#password.value = ''; }, 2000 ); //avoid blanking while page is shown
				} else {
					rej( result.error );
				}
			};
		} );
	}
}

class BrumeCallCe extends HTMLElement {
	constructor() {
		super();
		//this.hidden = true;
		this.attachShadow( { mode: 'open' } ).innerHTML = `
			<div style="display: flex; align-items: center; justify-content: left;">
				<button>connect</button>
				<button>disconnect</button>
				<input style="width: 10em;" type="text" placeholder = "Username to call" />
				<div id="idP" style="margin-left: auto; margin-right: 1em;"></div>
			</div>
		`;
		this.shadowRoot.adoptedStyleSheets = [ brumeStyleSheet ];
	}

	connectedCallback() {
		setTimeout( () => this.#configure(), 0 );
	}

	#configure(){
		this.shadowRoot.querySelectorAll( 'div button' )[ 0 ].style.display = '';
		this.shadowRoot.querySelectorAll( 'div button' )[ 1 ].style.display = 'none';
	}

	connected(){
		this.shadowRoot.querySelectorAll( 'div button' )[ 0 ].style.display = 'none';
		this.shadowRoot.querySelectorAll( 'div button' )[ 1 ].style.display = '';
	}

	disconnected(){
		this.shadowRoot.querySelectorAll( 'div button' )[ 0 ].style.display = '';
		this.shadowRoot.querySelectorAll( 'div button' )[ 1 ].style.display = 'none';
	}

	set callListener( f ){
		this.shadowRoot.querySelectorAll( 'div button' )[ 0 ].addEventListener( 'click', async ( e ) => {
			if( await f( e ) ){
				this.connected();
			}
		} );
	}

	set hangupListener( f ){
		this.shadowRoot.querySelectorAll( 'div button' )[ 1 ].addEventListener( 'click', ( e ) => {
			f( e );
			this.disconnected();
		} );
	}

	get name() {
		return this.shadowRoot.querySelector( 'input' ).value;
	}

	set name( n ) {
		this.shadowRoot.querySelector( 'input' ).value = n;
	}
}

/*** dialog ***/

class DialogCe extends HTMLElement {
	#cancelBtn;
	#OKBtn;
	#dialogMsg;
	#input;

	constructor() {
		super();
		// Attach a shadow root for style encapsulation
		this.attachShadow( { mode: 'open' } );
		this.hidden = true;
		this.shadowRoot.innerHTML = `
			<style>
					.dialog-container {
						position: absolute;
						top: 5vh;
						left: 50%;
						transform: translateX(-50%);
						display: flex;
						flex-direction: column;
						justify-content: center;
					}

					.dialog {
						width: fit-content;
						min-width: 10vw;
						max-width: 20vw;
						min-height: 4em;
						padding: 0.75em 0.75em 0.75em 0.75em;
						border: 1px solid #ccc;
						background: #fff;
						box-shadow: 0 4px 10px rgba(0, 0, 0, 0.1);
						border-radius: 6px;
						z-index: 9999;
					}

					#dialogMsg {
						max-width: 40vw;
						word-wrap: break-word;
						text-align: left;
						margin-left: auto;
						margin-right: auto;
					}

					span.dialog-btns {
						float: right;
					}

					.cancelBtn {
						height: 1.75em;
						font-size: 1em;
						border: lightgray solid 1px;
						background-color: white;
						border-radius: 8px;
						/*visibility: hidden;*/
					}

					.OKBtn {
						height: 1.75em;
						font-size: 1em;
						background-color: red;
						color: white;
						border: #fafafa solid 1px;
						border-radius: 8px;
					}
				</style>
				<div class="dialog-container">
					<div class="dialog">
						<div id="dialogMsg" style="margin-bottom: 1em;"></div>
						<div id="prompt" hidden>
							<input  type="text" id="fname" name="fname"></br></br>
						</div>
						<span class="dialog-btns">
							<button id="cancelBtn" class="cancelBtn">Cancel</button>
							<button id="OKBtn" class="OKBtn">OK</button>
						</span>
					</div>
				</div>
			`;
		this.#cancelBtn = this.shadowRoot.querySelector( "#cancelBtn" );
		this.#OKBtn = this.shadowRoot.querySelector( "#OKBtn" );
		this.#dialogMsg = this.shadowRoot.querySelector( "#dialogMsg" );
		this.#input = this.shadowRoot.querySelector( "#prompt" );
	}

	#dialog( type, msg ){
		return new Promise( ( res, rej ) => {
			this.#dialogMsg.innerHTML = msg;
			this.#cancelBtn.style.visibility = type == 'alert' ? 'hidden' : 'visible';
			this.#input.hidden = type === 'prompt' ? false : true;
			this.hidden = false;
			this.shadowRoot.querySelector( '#fname' ).focus();

			const btnHandler = ( e ) => {
				this.hidden = true;
				this.#cancelBtn.removeEventListener( 'click', btnHandler );
				this.#OKBtn.removeEventListener( 'click', btnHandler );
				res( e.currentTarget.firstChild.data == 'OK'
					? ( type === 'prompt' ? this.#input.childNodes[1].value : true )
					: ( type === 'prompt' ? null : false ) );
			};
			this.#cancelBtn.addEventListener( 'click', btnHandler );
			this.#OKBtn.addEventListener( 'click', btnHandler );
		} );
	};

	async alert( msg ){ return await this.#dialog( 'alert', msg ); };
	async confirm( msg ){ return await this.#dialog( 'confirm', msg ); };
	async prompt( msg ){ return await this.#dialog( 'prompt', msg ); };
}

