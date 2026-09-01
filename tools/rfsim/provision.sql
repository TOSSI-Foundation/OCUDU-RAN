USE oai_db;
DROP PROCEDURE IF EXISTS prov;
DELIMITER //
CREATE PROCEDURE prov(IN lo INT, IN hi INT)
BEGIN
  DECLARE i INT DEFAULT lo;
  WHILE i <= hi DO
    INSERT IGNORE INTO AuthenticationSubscription
      SELECT LPAD(CONCAT('00101', LPAD(i,10,'0')),15,'0'), authenticationMethod, encPermanentKey,
             protectionParameterId, sequenceNumber, authenticationManagementField, algorithmId,
             encOpcKey, encTopcKey, vectorGenerationInHss, n5gcAuthMethod, rgAuthenticationInd,
             LPAD(CONCAT('00101', LPAD(i,10,'0')),15,'0')
      FROM AuthenticationSubscription WHERE ueid='001010000000001';
    INSERT IGNORE INTO SessionManagementSubscriptionData
      SELECT LPAD(CONCAT('00101', LPAD(i,10,'0')),15,'0'), servingPlmnid, singleNssai,
             JSON_SET(dnnConfigurations, '$.oai.staticIpAddress[0].ipv4Addr', CONCAT('10.0.0.', i+1)),
             internalGroupIds, sharedVnGroupDataIds, sharedDnnConfigurationsId,
             odbPacketServices, traceData, sharedTraceDataId, expectedUeBehavioursList,
             suggestedPacketNumDlList, `3gppChargingCharacteristics`
      FROM SessionManagementSubscriptionData WHERE ueid='001010000000001';
    SET i = i + 1;
  END WHILE;
END//
DELIMITER ;
CALL prov(5, 60);
DROP PROCEDURE prov;
SELECT COUNT(*) AS auth FROM AuthenticationSubscription;
SELECT COUNT(*) AS sm FROM SessionManagementSubscriptionData;
