// SPDX-License-Identifier: MIT
pragma solidity ^0.8.24;

import {Test} from "forge-std/Test.sol";
import {WQTC, IComplianceHook} from "../evm/WQTC.sol";
import {WQTCBridge, ECDSAMultisigVerifier, IAttestationVerifier} from "../evm/WQTCBridge.sol";
import {WQTCAtomicSwapHTLC} from "../evm/WQTCAtomicSwapHTLC.sol";
import {IERC20} from "@openzeppelin/contracts/token/ERC20/IERC20.sol";

contract WQTCTest is Test {
    WQTC wqtc;
    WQTCBridge bridge;
    ECDSAMultisigVerifier verifier;

    address admin = address(0xA11CE);
    address gov   = address(0x60F);     // governance (timelock stand-in)
    address guardian = address(0x6A7D);
    address pauser = address(0x9A05E);
    // three federation signers (sorted ascending by address for the proof)
    uint256 pk1 = 0xA11; uint256 pk2 = 0xB22; uint256 pk3 = 0xC33;

    function setUp() public {
        address[] memory signers = new address[](3);
        signers[0] = vm.addr(pk1); signers[1] = vm.addr(pk2); signers[2] = vm.addr(pk3);
        verifier = new ECDSAMultisigVerifier(admin, 0, signers, 2); // 2-of-3
        wqtc = new WQTC(admin, 0);
        bridge = new WQTCBridge(wqtc, IAttestationVerifier(address(verifier)), 1, admin, 0);

        vm.startPrank(admin);
        wqtc.grantRole(wqtc.MINTER_ROLE(), address(bridge));
        wqtc.grantRole(wqtc.BURNER_ROLE(), address(bridge));
        wqtc.grantRole(wqtc.PAUSER_ROLE(), pauser);
        wqtc.grantRole(wqtc.UNPAUSER_ROLE(), admin);
        bridge.grantRole(bridge.GOVERNANCE_ROLE(), gov);
        bridge.grantRole(bridge.GUARDIAN_ROLE(), guardian);
        bridge.grantRole(bridge.PAUSER_ROLE(), pauser);
        bridge.grantRole(bridge.FEDERATION_ROLE(), gov);
        vm.stopPrank();
    }

    // --- helpers ---

    function _attest(bytes32 txid, uint32 vout, address to, uint64 amtSat) internal view returns (bytes memory) {
        bytes32 digest = bridge.mintDigest(txid, vout, to, amtSat);
        // sign with pk1 and pk2, then order by recovered address ascending
        (uint8 v1, bytes32 r1, bytes32 s1) = vm.sign(pk1, digest);
        (uint8 v2, bytes32 r2, bytes32 s2) = vm.sign(pk2, digest);
        bytes memory sigA = abi.encodePacked(r1, s1, v1);
        bytes memory sigB = abi.encodePacked(r2, s2, v2);
        bytes[] memory sigs = new bytes[](2);
        if (vm.addr(pk1) < vm.addr(pk2)) { sigs[0] = sigA; sigs[1] = sigB; }
        else { sigs[0] = sigB; sigs[1] = sigA; }
        return abi.encode(sigs);
    }

    // --- mint ---

    function test_MintHappyPath() public {
        bytes32 txid = keccak256("dep1");
        bytes memory proof = _attest(txid, 0, address(0xBEEF), 100_000_000); // 1 QTC
        bridge.mint(txid, 0, address(0xBEEF), 100_000_000, proof);
        assertEq(wqtc.balanceOf(address(0xBEEF)), 100_000_000 * 1e10);
        assertEq(wqtc.totalSupply(), 1e18);
    }

    function test_OutpointReplayRejected() public {
        bytes32 txid = keccak256("dep2");
        bytes memory proof = _attest(txid, 0, address(0xBEEF), 1_000);
        bridge.mint(txid, 0, address(0xBEEF), 1_000, proof);
        vm.expectRevert(WQTCBridge.AlreadyMinted.selector);
        bridge.mint(txid, 0, address(0xBEEF), 1_000, proof);
    }

    function test_EIP712Replay_WrongRecipientRejected() public {
        bytes32 txid = keccak256("dep3");
        bytes memory proof = _attest(txid, 0, address(0xBEEF), 1_000); // signed for 0xBEEF
        vm.expectRevert(WQTCBridge.BadAttestation.selector);
        bridge.mint(txid, 0, address(0xDEAD), 1_000, proof);           // try to redirect
    }

    function test_BelowThresholdRejected() public {
        bytes32 txid = keccak256("dep4");
        bytes32 digest = bridge.mintDigest(txid, 0, address(0xBEEF), 1_000);
        (uint8 v1, bytes32 r1, bytes32 s1) = vm.sign(pk1, digest);
        bytes[] memory sigs = new bytes[](1);
        sigs[0] = abi.encodePacked(r1, s1, v1);
        vm.expectRevert(WQTCBridge.BadAttestation.selector);
        bridge.mint(txid, 0, address(0xBEEF), 1_000, abi.encode(sigs)); // 1-of-3 < threshold 2
    }

    // --- circuit breaker + guardian veto ---

    function test_WindowCapEnforced() public {
        vm.prank(gov);
        bridge.setLimits(0, 50_000, 1 days, 0, 0, 7 days); // window cap 50k sat
        bytes32 txid = keccak256("dep5");
        bytes memory proof = _attest(txid, 0, address(0xBEEF), 60_000);
        vm.expectRevert(WQTCBridge.WindowCapExceeded.selector);
        bridge.mint(txid, 0, address(0xBEEF), 60_000, proof);
    }

    function test_GuardianVetoFlow() public {
        vm.prank(gov);
        bridge.setLimits(0, 0, 1 days, 100_000, 1 hours, 7 days); // queue mints > 100k sat
        bytes32 txid = keccak256("dep6");
        bytes memory proof = _attest(txid, 7, address(0xBEEF), 500_000);
        bridge.mint(txid, 7, address(0xBEEF), 500_000, proof);      // queued, not minted
        assertEq(wqtc.balanceOf(address(0xBEEF)), 0);
        // too early
        vm.expectRevert(WQTCBridge.TooEarly.selector);
        bridge.executeQueuedMint(txid, 7);
        // guardian cancels
        vm.prank(guardian);
        bridge.cancelQueuedMint(txid, 7);
        vm.warp(block.timestamp + 2 hours);
        vm.expectRevert(WQTCBridge.NotQueued.selector);
        bridge.executeQueuedMint(txid, 7);
        assertEq(wqtc.balanceOf(address(0xBEEF)), 0);              // never minted
    }

    function test_GuardianQueueExecutesAfterDelay() public {
        vm.prank(gov);
        bridge.setLimits(0, 0, 1 days, 100_000, 1 hours, 7 days);
        bytes32 txid = keccak256("dep7");
        bytes memory proof = _attest(txid, 0, address(0xBEEF), 500_000);
        bridge.mint(txid, 0, address(0xBEEF), 500_000, proof);
        vm.warp(block.timestamp + 2 hours);
        bridge.executeQueuedMint(txid, 0);
        assertEq(wqtc.balanceOf(address(0xBEEF)), 500_000 * 1e10);
    }

    // --- redeem ---

    function test_RedeemRoundDownAndRefund() public {
        bytes32 txid = keccak256("dep8");
        bytes memory proof = _attest(txid, 0, address(this), 200_000_000); // mint 2 QTC
        bridge.mint(txid, 0, address(this), 200_000_000, proof);
        // redeem 1 QTC + 0.5 sat dust -> releases 100000000 sat (rounded down), burns the full amount
        uint256 amt = 100_000_000 * 1e10 + 5e9;
        uint256 id = bridge.redeem(amt, hex"00aabb");
        assertEq(wqtc.totalSupply(), 2e18 - amt);                  // full burn incl dust
        ( , uint64 sat, , , bool fulfilled, ) = bridge.redeems(id);
        assertEq(sat, 100_000_000);                                // rounded down
        assertEq(fulfilled, false);
        // refund after timeout re-mints the burned amount to the burner
        vm.warp(block.timestamp + 8 days);
        vm.prank(gov);
        bridge.refundRedeem(id);
        assertEq(wqtc.balanceOf(address(this)), 2e18);             // made whole
    }

    // --- token roles / rescue / pause ---

    function test_OnlyBridgeMints() public {
        vm.expectRevert();
        wqtc.mint(address(this), 1);                                // not MINTER_ROLE
    }

    function test_IssuancePauseBlocksMint() public {
        vm.prank(pauser);
        wqtc.pauseIssuance();
        bytes32 txid = keccak256("dep9");
        bytes memory proof = _attest(txid, 0, address(0xBEEF), 1_000);
        vm.expectRevert(WQTC.IssuanceIsPaused.selector);
        bridge.mint(txid, 0, address(0xBEEF), 1_000, proof);
    }

    function test_RescueCannotTouchSelf() public {
        bytes32 rescuer = wqtc.RESCUER_ROLE();                      // cache (avoid consuming the prank)
        vm.startPrank(admin);
        wqtc.grantRole(rescuer, admin);
        vm.expectRevert(WQTC.CannotRescueSelf.selector);
        wqtc.rescueERC20(IERC20(address(wqtc)), admin, 0);
        vm.stopPrank();
    }

    function test_VerifierGovernanceGated() public {
        vm.expectRevert();
        bridge.setVerifier(IAttestationVerifier(address(0xdead)));  // not GOVERNANCE_ROLE
    }

    function test_GranularPause() public {
        bytes32 txid = keccak256("gp");
        bytes memory proof = _attest(txid, 0, address(this), 1_000);
        vm.prank(pauser); bridge.setMintPaused(true);
        vm.expectRevert(WQTCBridge.MintPaused.selector);
        bridge.mint(txid, 0, address(this), 1_000, proof);          // mint blocked
        vm.prank(pauser); bridge.setMintPaused(false);
        bridge.mint(txid, 0, address(this), 1_000, proof);          // mint works again
        vm.prank(pauser); bridge.setRedeemPaused(true);
        vm.expectRevert(WQTCBridge.RedeemPaused.selector);
        bridge.redeem(1_000 * 1e10, hex"00aa");                     // redeem blocked independently
    }

    // backing relation: minting `amountSat` yields exactly amountSat*1e10 wQTC (no phantom supply).
    function testFuzz_BackingRelation(uint64 amountSat) public {
        amountSat = uint64(bound(amountSat, 1, 2_100_000_000_000_000)); // up to 21M QTC in sat
        bytes32 txid = keccak256(abi.encode("fuzz", amountSat));
        bytes memory proof = _attest(txid, 3, address(0xBEEF), amountSat);
        bridge.mint(txid, 3, address(0xBEEF), amountSat, proof);
        assertEq(wqtc.totalSupply(), uint256(amountSat) * 1e10);
        assertEq(wqtc.balanceOf(address(0xBEEF)), wqtc.totalSupply());
    }
}

contract MockBlockHook is IComplianceHook {
    address public blocked;
    constructor(address b) { blocked = b; }
    function check(address from, address to, uint256) external view {
        require(from != blocked && to != blocked, "blocked");
    }
}

contract WQTCTokenTest is Test {
    WQTC wqtc;
    address admin = address(0xA11CE);
    uint256 alicePk = 0xA11CE5;
    address alice;
    address bob = address(0xB0B);

    function setUp() public {
        alice = vm.addr(alicePk);
        wqtc = new WQTC(admin, 0);
        vm.startPrank(admin);
        wqtc.grantRole(wqtc.MINTER_ROLE(), admin);
        wqtc.mint(alice, 1000e18);
        vm.stopPrank();
    }

    function _digest(bytes32 structHash) internal view returns (bytes32) {
        return keccak256(abi.encodePacked("\x19\x01", wqtc.DOMAIN_SEPARATOR(), structHash));
    }

    function test_EIP3009Transfer_AndReplayRejected() public {
        uint256 vb = block.timestamp + 1 hours;
        bytes32 nonce = keccak256("n1");
        bytes32 sh = keccak256(abi.encode(wqtc.TRANSFER_WITH_AUTHORIZATION_TYPEHASH(), alice, bob, 10e18, uint256(0), vb, nonce));
        (uint8 v, bytes32 r, bytes32 s) = vm.sign(alicePk, _digest(sh));
        wqtc.transferWithAuthorization(alice, bob, 10e18, 0, vb, nonce, v, r, s); // relayer submits
        assertEq(wqtc.balanceOf(bob), 10e18);
        vm.expectRevert(WQTC.AuthUsedOrCanceled.selector);
        wqtc.transferWithAuthorization(alice, bob, 10e18, 0, vb, nonce, v, r, s); // replay
    }

    function test_EIP3009Receive_OnlyPayee() public {
        uint256 vb = block.timestamp + 1 hours;
        bytes32 nonce = keccak256("n2");
        bytes32 sh = keccak256(abi.encode(wqtc.RECEIVE_WITH_AUTHORIZATION_TYPEHASH(), alice, bob, 5e18, uint256(0), vb, nonce));
        (uint8 v, bytes32 r, bytes32 s) = vm.sign(alicePk, _digest(sh));
        vm.expectRevert(WQTC.CallerMustBePayee.selector);
        wqtc.receiveWithAuthorization(alice, bob, 5e18, 0, vb, nonce, v, r, s); // wrong caller (this)
        vm.prank(bob);
        wqtc.receiveWithAuthorization(alice, bob, 5e18, 0, vb, nonce, v, r, s); // payee submits
        assertEq(wqtc.balanceOf(bob), 5e18);
    }

    function test_ComplianceHookBlocks() public {
        MockBlockHook hook = new MockBlockHook(bob);
        vm.prank(admin);
        wqtc.setComplianceHook(hook);
        vm.prank(alice);
        vm.expectRevert(bytes("blocked"));
        wqtc.transfer(bob, 1e18);
        // removing the hook restores neutrality
        vm.prank(admin);
        wqtc.setComplianceHook(IComplianceHook(address(0)));
        vm.prank(alice);
        wqtc.transfer(bob, 1e18);
        assertEq(wqtc.balanceOf(bob), 1e18);
    }

    function test_Permit() public {
        uint256 deadline = block.timestamp + 1 hours;
        bytes32 sh = keccak256(abi.encode(
            keccak256("Permit(address owner,address spender,uint256 value,uint256 nonce,uint256 deadline)"),
            alice, bob, 7e18, wqtc.nonces(alice), deadline));
        (uint8 v, bytes32 r, bytes32 s) = vm.sign(alicePk, _digest(sh));
        wqtc.permit(alice, bob, 7e18, deadline, v, r, s);
        assertEq(wqtc.allowance(alice, bob), 7e18);
    }
}

contract HTLCTest is Test {
    WQTCAtomicSwapHTLC htlc;
    WQTC token;
    address admin = address(0xA11CE);
    address alice = address(0xA11);   // funds the swap
    address bob   = address(0xB0B);   // claims with preimage

    function setUp() public {
        htlc = new WQTCAtomicSwapHTLC();
        token = new WQTC(admin, 0);
        vm.startPrank(admin);
        token.grantRole(token.MINTER_ROLE(), admin);
        token.mint(alice, 1000e18);
        vm.stopPrank();
    }

    function test_HashDomainMatchesQTC() public view {
        // QTC OP_HASH160 of 0x42*32 (verified against a QTC node) == 8739f40e...566981
        bytes memory pre = new bytes(32);
        for (uint i; i < 32; i++) pre[i] = 0x42;
        assertEq(htlc.qtcHash160(pre), bytes20(hex"8739f40ec4dbf569dcb38134c6e7310908566981"));
    }

    function test_ClaimWithPreimage() public {
        bytes memory pre = new bytes(32);
        for (uint i; i < 32; i++) pre[i] = 0x42;
        bytes20 h = htlc.qtcHash160(pre);
        vm.startPrank(alice);
        token.approve(address(htlc), 100e18);
        bytes32 id = htlc.open(bob, address(token), 100e18, h, uint64(block.timestamp + 1 hours), bytes32("s"));
        vm.stopPrank();
        vm.prank(bob);
        htlc.claim(id, pre);
        assertEq(token.balanceOf(bob), 100e18);
    }

    function test_RefundAfterTimeout() public {
        bytes20 h = bytes20(hex"8739f40ec4dbf569dcb38134c6e7310908566981");
        vm.startPrank(alice);
        token.approve(address(htlc), 100e18);
        bytes32 id = htlc.open(bob, address(token), 100e18, h, uint64(block.timestamp + 1 hours), bytes32("s"));
        vm.stopPrank();
        vm.warp(block.timestamp + 2 hours);
        vm.prank(alice);
        htlc.refund(id);
        assertEq(token.balanceOf(alice), 1000e18);
    }
}
