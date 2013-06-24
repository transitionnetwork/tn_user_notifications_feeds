<?php
/**
 * @file
 * FeedsUserNotificationProcessor class.
 */

// Rob Hopkins UID
define('ROB_HOPKINS_UID', 4740);
// Rob Hopkins UID
define('TRANSITION_NEWSLETTER_MC_LID', '33e8648c8d');

/**
 * Feeds processor plugin. Create users from feed items.
 */
class FeedsUserNotificationProcessor extends FeedsProcessor {

  /**
   * Implementation of FeedsProcessor::process().
   */
  public function process(FeedsImportBatch $batch, FeedsSource $source) {

    // Count number of created and updated nodes.
    $created = $updated = $failed = 0;

    // prep CSV
    $export = array(
      'updated' => array(),
      'created' => array(),
    );

    // do import of user
    while ($item = $batch->shiftItem()) {

      // Ensure this runs for up to 2 mins.
      drupal_set_time_limit(120);

      // Check existing
      $uid = $this->existingItemId($batch, $source);

      // Map item to a term.
      $account = $this->map($batch);

      // Watchdog logging entry.
      $results = array();

      // Existing = don't update users & profiles for existing people.
      if (!$uid) {

        // Generate usename, clear names values from $account ready for save.
        $account->name = $this->generateUsername($account);
        $results['name'] = $account->name;
        $first_name = $account->first;
        unset($account->first);
        $last_name = $account->last;
        unset($account->last);

        // Check if user name and mail are set, otherwise skip/continue.
        if (empty($account->name) || empty($account->mail) || !valid_email_address($account->mail)) {
          $failed++;
          continue;
        }

        // Save new user & reload.
        user_save($account, (array) $account);
        if (!$account->uid) {
          $account = user_load(array('mail' => $account->mail));
        }
        $results['uid'] = $account->uid;

        // Create Profile node if needs be.
        $results['nid'] = $this->generateProfileNode($account, $first_name, $last_name);

        // Bump counts.
        $created++;
        $results['action'] = 'created';

      }
      else {
        // Got user already, load account.
        $account = user_load(array('mail' => $account->mail));
        $results['name'] = $account->name;
        $results['uid'] = $account->uid;

        // Bump counts.
        $updated++;
        $results['action'] = 'updated';
      }

      // Now add Mailchimp and Notificationsusers & subs here
      $results['subscription status'] = $this->createNotification($account);
      $results['mailchimp status'] = $this->createMailchimpSubscription($account);

      // make one-time link for created users
      $onetime = '';
      if ($results['action'] == 'created') {
        $onetime = user_pass_reset_url($account);
      }

      // add line to export for CSV
      $date = format_date(time() + 86400, 'small');
      $export[$results['action']][] = $account->uid .',"'. $account->name .'","'. $account->mail .'","'. $onetime .'","'. ($account->login ? $date : '') ."\"";

      // Write log for this user.
      watchdog(
        'FeedsUserNotificationProcessor',
        $results['action'] . ': ' . print_r($results, TRUE)
      );

    }
    // prep headers for export,
    $head = t('UID') .','. t('Name') .','. t('Mail') .','. t('URL') .','. t('Expiry') ."\r\n";
    watchdog(
      'FeedsUserNotificationProcessor',
        'All: ' . ': ' . print_r($export, TRUE)
    );
    // Output updated list as csv.
    watchdog(
      'FeedsUserNotificationProcessor',
      'Updated' . $head . implode("\r\n", $export['updated'])
    );

    // Output created list as csv.
    watchdog(
      'FeedsUserNotificationProcessor',
      'Created' . $head . implode("\r\n", $export['created'])
    );

    // Set messages.
    if ($failed) {
      drupal_set_message(
        format_plural(
          $failed,
          'There was @number user that could not be imported because either their name or their email was empty or not valid. Check import data and mapping settings on User processor.',
          'There were @number users that could not be imported because either their name or their email was empty or not valid. Check import data and mapping settings on User processor.',
          array('@number' => $failed)
        ),
        'error'
      );
    }
    if ($created) {
      drupal_set_message(format_plural($created, 'Created @number user.', 'Created @number users.', array('@number' => $created)));
    }
    elseif ($updated) {
      drupal_set_message(format_plural($updated, 'Updated @number user.', 'Updated @number users.', array('@number' => $updated)));
    }
    else {
      drupal_set_message(t('There are no new users.'));
    }
  }

  /**
   * Generates user profile node if one does not exist for $account.
   */
  private function generateProfileNode($account, $first_name, $last_name) {
    $type_name = 'profile';
    // Try to load existing node.
    $profile_node = content_profile_load($type_name, $account->uid);
    if (empty($profile_node)) {
      // None existing, generate new one
      $profile_node = array(
        'uid' => $account->uid,
        'name' => isset($account->name) ? $account->name : '',
        'type' => $type_name,
        'language' => '',
      );
      $profile_node = (object) $profile_node;

      // JK set name fields, save new profile
      $profile_node->field_name_first[0]['value'] = $first_name;
      $profile_node->field_name_last[0]['value'] = $last_name;
      node_save($profile_node);
    }
    // Return nid regardless.
    return $profile_node->nid;
  }

  /**
   * NOTIFICATIONS - Notifications sub add for Rob's blog.
   */
  private function createNotification($account) {
    // Get defaults, set up basics.
    $subscription = notifications_build_subscription(array(
      'uid' => $account->uid,
      'type' => 'typeauthor',
      'send_interval' => notifications_user_setting('send_interval', $account, 0),
      'send_method' => notifications_user_setting('send_method', $account),
    ));
    // build sub, add fields for author and type
    $subscription->add_field_args('author,type', ROB_HOPKINS_UID . ',blog');
    $subscription->set_account($account);
    return notifications_save_subscription($subscription);
  }


  /**
   * NOTIFICATIONS - Notifications sub add for Rob's blog.
   */
  private function createMailchimpSubscription($account) {
    // MAILCHIMP
    // MailChimp newsletter
    if ($q = _mailchimp_get_api_object()) {
      // subscribe
      $lists = _mailchimp_get_available_lists($account);
      $list = $lists[TRANSITION_NEWSLETTER_MC_LID];
      $merge_vars = _mailchimp_load_user_list_mergevars($account->uid, TRANSITION_NEWSLETTER_MC_LID, $q->listMergeVars(TRANSITION_NEWSLETTER_MC_LID));

      return _mailchimp_subscribe_user($list, $account->mail, $merge_vars, TRUE, $q);
    }
  }
  // Builds a unique username based on first & last name, avoiding collisions
  // @see https://github.com/transitionnetwork/transition_pse/blob/master/transition_pse.module
  // @todo check more 'drupally' way
  public function generateUsername($account) {
    // build username from first & last names
    $first_name = trim($account->first);
    $last_name = trim($account->last);

    // make alphanumeric only, set proper noun capitalisation
    $full_name = ucwords(strtolower($first_name . ' ' . $last_name));
    $full_name = preg_replace("/[^a-zA-Z0-9\s]+/", "", $full_name);

    // then ensure not too long after making space for 'counter' on end
    if (strlen($full_name) >= USERNAME_MAX_LENGTH - 3) $name = substr($name, 0, USERNAME_MAX_LENGTH - 3);

    // ensure username is unique
    $attempt = 0;
    $append = '';
    $success = FALSE;
    while (!$success && $attempt < 256) {
      // add attempt number to end of username if not unique - e.g. "John Smith 2"
      if ($attempt > 0) $append = ' ' . dechex($attempt);

      $result = db_query("SELECT name FROM {users} u WHERE u.name = '%s'", $full_name . $append);
      if (db_fetch_array($result) === FALSE) {
        // found a unique name, use it!
        return $full_name . $append;
        ;
      }
      // otherwise carry on up to 0xff appended to name... or 256 chances.
      $attempt++;
    }
    // fall back to firstname and jibberish.
    return $first_name . uniqid();
  }


  /**
   * Implementation of FeedsProcessor::clear().
   */
  public function clear(FeedsBatch $batch, FeedsSource $source) {
    // Do not support deleting users as we have no way of knowing which ones we
    // imported.
    throw new Exception(t('User processor does not support deleting users.'));
  }

  /**
   * Execute mapping on an item.
   */
  protected function map(FeedsImportBatch $batch, $target_account = NULL) {
    // Prepare user account object.
    if (empty($target_account)) {
      $target_account = new stdClass();
    }

    $target_account->uid = 0;
    $target_account->roles = array_filter($this->config['roles']);
    $target_account->status = $this->config['status'];

    // Have parent class do the iterating.
    return parent::map($batch, $target_account);
  }

  /**
   * Override parent::configDefaults().
   */
  public function configDefaults() {
    return array(
      'roles' => array(),
      'update_existing' => FALSE,
      'status' => 1,
      'mappings' => array(),
    );
  }

  /**
   * Override parent::configForm().
   */
  public function configForm(&$form_state) {
    $form = array();

    $form['status'] = array(
      '#type' => 'radios',
      '#title' => t('Status'),
      '#description' => t('Select whether users should be imported active or blocked.'),
      '#options' => array(0 => t('Blocked'), 1 => t('Active')),
      '#default_value' => $this->config['status'],
    );

    $roles = user_roles(TRUE);
    unset($roles[2]);
    if (count($roles)) {
      $form['roles'] = array(
        '#type' => 'checkboxes',
        '#title' => t('Additional roles'),
        '#description' => t('Every user is assigned the "authenticated user" role. Select additional roles here.'),
        '#default_value' => $this->config['roles'],
        '#options' => $roles,
      );
    }
    return $form;
  }

  /**
   * Set target element.
   */
  public function setTargetElement(&$target_item, $target_element, $value) {
    $target_item->$target_element = $value;
  }

  /**
   * Return available mapping targets.
   */
  public function getMappingTargets() {
    $targets = array(
      'name' => array(
        'name' => t('User name'),
        'description' => t('Name of the user.'),
        'optional_unique' => TRUE,
       ),
      'mail' => array(
        'name' => t('Email address'),
        'description' => t('Email address of the user.'),
        'optional_unique' => TRUE,
       ),
      'created' => array(
        'name' => t('Created date'),
        'description' => t('The created (e. g. joined) data of the user.'),
       ),
      // JK: First and last names
      'first' => array(
        'name' => t('First name'),
        'description' => t('The user\'s first name.'),
      ),
      'last' => array(
        'name' => t('Last name'),
        'description' => t('The user\'s last name.'),
      ),
    );
    // Let other modules expose mapping targets.
    self::loadMappers();
    drupal_alter('feeds_user_processor_targets', $targets);

    return $targets;
  }

  /**
   * Get id of an existing feed item term if available.
   */
  protected function existingItemId(FeedsImportBatch $batch, FeedsSource $source) {

    // Iterate through all unique targets and try to find a user for the
    // target's value.
    foreach ($this->uniqueTargets($batch) as $target => $value) {
      switch ($target) {
        case 'name':
          $uid = db_result(db_query("SELECT uid FROM {users} WHERE name = '%s'", $value));
          break;
        case 'mail':
          $uid = db_result(db_query("SELECT uid FROM {users} WHERE mail = '%s'", $value));
          break;
        case 'openid':
          $uid = db_result(db_query("SELECT uid FROM {authmap} WHERE authname = '%s' AND module = 'openid'", $value));
          break;
      }
      if ($uid) {
        // Return with the first nid found.
        return $uid;
      }
    }
    return 0;
  }
}
